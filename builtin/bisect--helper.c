#include "builtin.h"
#include "cache.h"
#include "parse-options.h"
#include "bisect.h"
#include "refs.h"
#include "dir.h"
#include "argv-array.h"
#include "run-command.h"
#include "prompt.h"

static GIT_PATH_FUNC(git_path_bisect_terms, "BISECT_TERMS")
static GIT_PATH_FUNC(git_path_bisect_expected_rev, "BISECT_EXPECTED_REV")
static GIT_PATH_FUNC(git_path_bisect_ancestors_ok, "BISECT_ANCESTORS_OK")
static GIT_PATH_FUNC(git_path_bisect_start, "BISECT_START")
static GIT_PATH_FUNC(git_path_bisect_head, "BISECT_HEAD")
static GIT_PATH_FUNC(git_path_bisect_log, "BISECT_LOG")

static const char * const git_bisect_helper_usage[] = {
	N_("git bisect--helper --next-all [--no-checkout]"),
	N_("git bisect--helper --write-terms <bad_term> <good_term>"),
	N_("git bisect--helper --bisect-clean-state"),
	N_("git bisect--helper --bisect-reset [<commit>]"),
	N_("git bisect--helper --bisect-write <state> <revision> <good_term> <bad_term> [<nolog>]"),
	N_("git bisect--helper --bisect-check-and-set-terms <command> <good_term> <bad_term>"),
	N_("git bisect--helper --bisect-next-check [<term>] <good_term> <bad_term>"),
	N_("git bisect--helper --bisect-terms [--term-good | --term-old | --term-bad | --term-new]"),
	NULL
};

struct bisect_terms {
	const char *term_good;
	const char *term_bad;
};

static void free_terms(struct bisect_terms *terms)
{
	if (!terms->term_good)
		free((void *) terms->term_good);
	if (!terms->term_bad)
		free((void *) terms->term_bad);
}

static void set_terms(struct bisect_terms *terms, const char *bad,
		      const char *good)
{
	terms->term_good = xstrdup(good);
	terms->term_bad = xstrdup(bad);
}

static const char *voc[] = {
	"bad|new",
	"good|old"
};

/*
 * Check whether the string `term` belongs to the set of strings
 * included in the variable arguments.
 */
LAST_ARG_MUST_BE_NULL
static int one_of(const char *term, ...)
{
	int res = 0;
	va_list matches;
	const char *match;

	va_start(matches, term);
	while (!res && (match = va_arg(matches, const char *)))
		res = !strcmp(term, match);
	va_end(matches);

	return res;
}

static int check_term_format(const char *term, const char *orig_term)
{
	int res;
	char *new_term = xstrfmt("refs/bisect/%s", term);

	res = check_refname_format(new_term, 0);
	free(new_term);

	if (res)
		return error(_("'%s' is not a valid term"), term);

	if (one_of(term, "help", "start", "skip", "next", "reset",
			"visualize", "replay", "log", "run", "terms", NULL))
		return error(_("can't use the builtin command '%s' as a term"), term);

	/*
	 * In theory, nothing prevents swapping completely good and bad,
	 * but this situation could be confusing and hasn't been tested
	 * enough. Forbid it for now.
	 */

	if ((strcmp(orig_term, "bad") && one_of(term, "bad", "new", NULL)) ||
		 (strcmp(orig_term, "good") && one_of(term, "good", "old", NULL)))
		return error(_("can't change the meaning of the term '%s'"), term);

	return 0;
}

static int write_terms(const char *bad, const char *good)
{
	FILE *fp = NULL;
	int res;

	if (!strcmp(bad, good))
		return error(_("please use two different terms"));

	if (check_term_format(bad, "bad") || check_term_format(good, "good"))
		return -1;

	fp = fopen(git_path_bisect_terms(), "w");
	if (!fp)
		return error_errno(_("could not open the file BISECT_TERMS"));

	res = fprintf(fp, "%s\n%s\n", bad, good);
	res |= fclose(fp);
	return (res < 0) ? -1 : 0;
}

static int is_expected_rev(const char *expected_hex)
{
	struct strbuf actual_hex = STRBUF_INIT;
	int res = 0;
	if (strbuf_read_file(&actual_hex, git_path_bisect_expected_rev(), 0) >= 40) {
		strbuf_trim(&actual_hex);
		res = !strcmp(actual_hex.buf, expected_hex);
	}
	strbuf_release(&actual_hex);
	return res;
}

static void check_expected_revs(const char **revs, int rev_nr)
{
	int i;

	for (i = 0; i < rev_nr; i++) {
		if (!is_expected_rev(revs[i])) {
			unlink_or_warn(git_path_bisect_ancestors_ok());
			unlink_or_warn(git_path_bisect_expected_rev());
		}
	}
}

static int bisect_reset(const char *commit)
{
	struct strbuf branch = STRBUF_INIT;

	if (!commit) {
		if (strbuf_read_file(&branch, git_path_bisect_start(), 0) < 1)
			return !printf(_("We are not bisecting.\n"));
		strbuf_rtrim(&branch);
	} else {
		struct object_id oid;

		if (get_oid_commit(commit, &oid))
			return error(_("'%s' is not a valid commit"), commit);
		strbuf_addstr(&branch, commit);
	}

	if (!file_exists(git_path_bisect_head())) {
		struct argv_array argv = ARGV_ARRAY_INIT;

		argv_array_pushl(&argv, "checkout", branch.buf, "--", NULL);
		if (run_command_v_opt(argv.argv, RUN_GIT_CMD)) {
			error(_("Could not check out original HEAD '%s'. Try "
				"'git bisect reset <commit>'."), branch.buf);
			strbuf_release(&branch);
			argv_array_clear(&argv);
			return -1;
		}
		argv_array_clear(&argv);
	}

	strbuf_release(&branch);
	return bisect_clean_state();
}

static void log_commit(FILE *fp, char *fmt, const char *state,
		       struct commit *commit)
{
	struct pretty_print_context pp = {0};
	struct strbuf commit_msg = STRBUF_INIT;
	char *label = xstrfmt(fmt, state);

	format_commit_message(commit, "%s", &commit_msg, &pp);

	fprintf(fp, "# %s: [%s] %s\n", label, oid_to_hex(&commit->object.oid),
		commit_msg.buf);

	strbuf_release(&commit_msg);
	free(label);
}

static int bisect_write(const char *state, const char *rev,
			const struct bisect_terms *terms, int nolog)
{
	struct strbuf tag = STRBUF_INIT;
	struct object_id oid;
	struct commit *commit;
	FILE *fp = NULL;
	int retval = 0;

	if (!strcmp(state, terms->term_bad)) {
		strbuf_addf(&tag, "refs/bisect/%s", state);
	} else if (one_of(state, terms->term_good, "skip", NULL)) {
		strbuf_addf(&tag, "refs/bisect/%s-%s", state, rev);
	} else {
		error(_("Bad bisect_write argument: %s"), state);
		goto fail;
	}

	if (get_oid(rev, &oid)) {
		error(_("couldn't get the oid of the rev '%s'"), rev);
		goto fail;
	}

	if (update_ref(NULL, tag.buf, &oid, NULL, 0,
		       UPDATE_REFS_MSG_ON_ERR))
		goto fail;

	fp = fopen(git_path_bisect_log(), "a");
	if (!fp) {
		error_errno(_("couldn't open the file '%s'"), git_path_bisect_log());
		goto fail;
	}

	commit = lookup_commit_reference(&oid);
	log_commit(fp, "%s", state, commit);

	if (!nolog)
		fprintf(fp, "git bisect %s %s\n", state, rev);

	goto finish;

fail:
	retval = -1;
finish:
	if (fp)
		fclose(fp);
	strbuf_release(&tag);
	return retval;
}

static int check_and_set_terms(struct bisect_terms *terms, const char *cmd)
{
	int has_term_file = !is_empty_or_missing_file(git_path_bisect_terms());

	if (one_of(cmd, "skip", "start", "terms", NULL))
		return 0;

	if (has_term_file && strcmp(cmd, terms->term_bad) &&
	    strcmp(cmd, terms->term_good))
		return error(_("Invalid command: you're currently in a "
				"%s/%s bisect"), terms->term_bad,
				terms->term_good);

	if (!has_term_file) {
		if (one_of(cmd, "bad", "good", NULL)) {
			free_terms(terms);
			set_terms(terms, "bad", "good");
			return write_terms(terms->term_bad, terms->term_good);
		}
		else if (one_of(cmd, "new", "old", NULL)) {
			free_terms(terms);
			set_terms(terms, "new", "old");
			return write_terms(terms->term_bad, terms->term_good);
		}
	}

	return 0;
}

static int mark_good(const char *refname, const struct object_id *oid,
		     int flag, void *cb_data)
{
	int *m_good = (int *)cb_data;
	*m_good = 0;
	return 1;
}

static int bisect_next_check(const struct bisect_terms *terms,
			     const char *current_term)
{
	int missing_good = 1, missing_bad = 1, retval = 0;
	const char *bad_ref = xstrfmt("refs/bisect/%s", terms->term_bad);
	const char *good_glob = xstrfmt("%s-*", terms->term_good);

	if (ref_exists(bad_ref))
		missing_bad = 0;

	for_each_glob_ref_in(mark_good, good_glob, "refs/bisect/",
			     (void *) &missing_good);

	if (!missing_good && !missing_bad)
		goto finish;

	if (!current_term)
		goto fail;

	if (missing_good && !missing_bad && current_term &&
	    !strcmp(current_term, terms->term_good)) {
		char *yesno;
		/*
		 * have bad (or new) but not good (or old). We could bisect
		 * although this is less optimum.
		 */
		fprintf(stderr, _("Warning: bisecting only with a %s commit\n"),
			terms->term_bad);
		if (!isatty(0))
			goto finish;
		/*
		 * TRANSLATORS: Make sure to include [Y] and [n] in your
		 * translation. The program will only accept English input
		 * at this point.
		 */
		yesno = git_prompt(_("Are you sure [Y/n]? "), PROMPT_ECHO);
		if (starts_with(yesno, "N") || starts_with(yesno, "n"))
			goto fail;

		goto finish;
	}
	if (!is_empty_or_missing_file(git_path_bisect_start())) {
		error(_("You need to give me at least one %s and "
			"%s revision. You can use \"git bisect %s\" "
			"and \"git bisect %s\" for that.\n"),
			voc[0], voc[1], voc[0], voc[1]);
		goto fail;
	} else {
		error(_("You need to start by \"git bisect start\". You "
			"then need to give me at least one %s and %s "
			"revision. You can use \"git bisect %s\" and "
			"\"git bisect %s\" for that.\n"),
			voc[1], voc[0], voc[1], voc[0]);
		goto fail;
	}
	goto finish;

fail:
	retval = -1;
finish:
	free((void *) good_glob);
	free((void *) bad_ref);
	return retval;
}

static int get_terms(struct bisect_terms *terms)
{
	struct strbuf str = STRBUF_INIT;
	FILE *fp = NULL;
	int res = 0;

	fp = fopen(git_path_bisect_terms(), "r");
	if (!fp)
		goto fail;

	free_terms(terms);
	strbuf_getline_lf(&str, fp);
	terms->term_bad = strbuf_detach(&str, NULL);
	strbuf_getline_lf(&str, fp);
	terms->term_good = strbuf_detach(&str, NULL);
	goto finish;

fail:
	res = -1;
finish:
	if (fp)
		fclose(fp);
	strbuf_release(&str);
	return res;
}

static int bisect_terms(struct bisect_terms *terms, const char **argv, int argc)
{
	int i;

	if (get_terms(terms))
		return error(_("no terms defined"));

	if (argc > 1)
		return error(_("--bisect-term requires exactly one argument"));

	if (argc == 0)
		return !printf(_("Your current terms are %s for the old state\n"
				 "and %s for the new state.\n"),
				 terms->term_good, terms->term_bad);

	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--term-good"))
			printf(_("%s\n"), terms->term_good);
		else if (!strcmp(argv[i], "--term-bad"))
			printf(_("%s\n"), terms->term_bad);
		else
			error(_("BUG: invalid argument %s for 'git bisect terms'.\n"
				  "Supported options are: "
				  "--term-good|--term-old and "
				  "--term-bad|--term-new."), argv[i]);
	}

	return 0;
}

int cmd_bisect__helper(int argc, const char **argv, const char *prefix)
{
	enum {
		NEXT_ALL = 1,
		WRITE_TERMS,
		BISECT_CLEAN_STATE,
		CHECK_EXPECTED_REVS,
		BISECT_RESET,
		BISECT_WRITE,
		CHECK_AND_SET_TERMS,
		BISECT_NEXT_CHECK,
		BISECT_TERMS
	} cmdmode = 0;
	int no_checkout = 0, res = 0;
	struct option options[] = {
		OPT_CMDMODE(0, "next-all", &cmdmode,
			 N_("perform 'git bisect next'"), NEXT_ALL),
		OPT_CMDMODE(0, "write-terms", &cmdmode,
			 N_("write the terms to .git/BISECT_TERMS"), WRITE_TERMS),
		OPT_CMDMODE(0, "bisect-clean-state", &cmdmode,
			 N_("cleanup the bisection state"), BISECT_CLEAN_STATE),
		OPT_CMDMODE(0, "check-expected-revs", &cmdmode,
			 N_("check for expected revs"), CHECK_EXPECTED_REVS),
		OPT_CMDMODE(0, "bisect-reset", &cmdmode,
			 N_("reset the bisection state"), BISECT_RESET),
		OPT_CMDMODE(0, "bisect-write", &cmdmode,
			 N_("update the refs according to the bisection state and may write it to BISECT_LOG"), BISECT_WRITE),
		OPT_CMDMODE(0, "check-and-set-terms", &cmdmode,
			 N_("check and set terms in a bisection state"), CHECK_AND_SET_TERMS),
		OPT_CMDMODE(0, "bisect-next-check", &cmdmode,
			 N_("check whether bad or good terms exist"), BISECT_NEXT_CHECK),
		OPT_CMDMODE(0, "bisect-terms", &cmdmode,
			 N_("print out the bisect terms"), BISECT_TERMS),
		OPT_BOOL(0, "no-checkout", &no_checkout,
			 N_("update BISECT_HEAD instead of checking out the current commit")),
		OPT_END()
	};
	struct bisect_terms terms;

	argc = parse_options(argc, argv, prefix, options,
			     git_bisect_helper_usage, PARSE_OPT_KEEP_UNKNOWN);

	if (!cmdmode)
		usage_with_options(git_bisect_helper_usage, options);

	switch (cmdmode) {
	int nolog;
	case NEXT_ALL:
		return bisect_next_all(prefix, no_checkout);
	case WRITE_TERMS:
		if (argc != 2)
			return error(_("--write-terms requires two arguments"));
		return write_terms(argv[0], argv[1]);
	case BISECT_CLEAN_STATE:
		if (argc != 0)
			return error(_("--bisect-clean-state requires no arguments"));
		return bisect_clean_state();
	case CHECK_EXPECTED_REVS:
		check_expected_revs(argv, argc);
		return 0;
	case BISECT_RESET:
		if (argc > 1)
			return error(_("--bisect-reset requires either no argument or a commit"));
		return bisect_reset(argc ? argv[0] : NULL);
	case BISECT_WRITE:
		if (argc != 4 && argc != 5)
			return error(_("--bisect-write requires either 4 or 5 arguments"));
		nolog = (argc == 5) && !strcmp(argv[4], "nolog");
		set_terms(&terms, argv[3], argv[2]);
		res = bisect_write(argv[0], argv[1], &terms, nolog);
		break;
	case CHECK_AND_SET_TERMS:
		if (argc != 3)
			return error(_("--check-and-set-terms requires 3 arguments"));
		set_terms(&terms, argv[2], argv[1]);
		res = check_and_set_terms(&terms, argv[0]);
		break;
	case BISECT_NEXT_CHECK:
		if (argc != 2 && argc != 3)
			return error(_("--bisect-next-check requires 2 or 3 arguments"));
		set_terms(&terms, argv[1], argv[0]);
		res = bisect_next_check(&terms, argc == 3 ? argv[2] : NULL);
		break;
	case BISECT_TERMS:
		if (argc > 1)
			return error(_("--bisect-terms requires 0 or 1 argument"));
		res = bisect_terms(&terms, argv, argc);
		break;
	default:
		return error("BUG: unknown subcommand '%d'", cmdmode);
	}
	free_terms(&terms);
	return res;
}
