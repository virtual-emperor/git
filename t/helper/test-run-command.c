/*
 * test-run-command.c: test run command API.
 *
 * (C) 2009 Ilari Liusvaara <ilari.liusvaara@elisanet.fi>
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "cache.h"
#include "run-command.h"
#include "argv-array.h"
#include "strbuf.h"
#include "parse-options.h"
#include "string-list.h"
#include "thread-utils.h"
#include "wildmatch.h"
#include <string.h>
#include <errno.h>

static int number_callbacks;
static int parallel_next(struct child_process *cp,
			 struct strbuf *err,
			 void *cb,
			 void **task_cb)
{
	struct child_process *d = cb;
	if (number_callbacks >= 4)
		return 0;

	argv_array_pushv(&cp->args, d->argv);
	strbuf_addstr(err, "preloaded output of a child\n");
	number_callbacks++;
	return 1;
}

static int no_job(struct child_process *cp,
		  struct strbuf *err,
		  void *cb,
		  void **task_cb)
{
	strbuf_addstr(err, "no further jobs available\n");
	return 0;
}

static int task_finished(int result,
			 struct strbuf *err,
			 void *pp_cb,
			 void *pp_task_cb)
{
	strbuf_addstr(err, "asking for a quick stop\n");
	return 1;
}

struct testsuite {
	struct string_list tests, failed;
	int next;
	int quiet, immediate, verbose, trace;
};

static int next_test(struct child_process *cp, struct strbuf *err, void *cb,
		     void **task_cb)
{
	struct testsuite *suite = cb;
	const char *test;
	if (suite->next >= suite->tests.nr)
		return 0;

	test = suite->tests.items[suite->next++].string;
	argv_array_pushl(&cp->args, "sh", test, NULL);
	if (suite->quiet)
		argv_array_push(&cp->args, "--quiet");
	if (suite->immediate)
		argv_array_push(&cp->args, "-i");
	if (suite->verbose)
		argv_array_push(&cp->args, "-v");
	if (suite->trace)
		argv_array_push(&cp->args, "-x");

	strbuf_addf(err, "Output of '%s':\n", test);
	*task_cb = (void *)test;

	return 1;
}

static int test_finished(int result, struct strbuf *err, void *cb,
			 void *task_cb)
{
	struct testsuite *suite = cb;
	const char *name = (const char *)task_cb;

	if (result)
		string_list_append(&suite->failed, name);

	strbuf_addf(err, "%s: '%s'\n", result ? "FAIL" : "SUCCESS", name);

	return 0;
}

static int test_failed(struct strbuf *out, void *cb, void *task_cb)
{
	struct testsuite *suite = cb;
	const char *name = (const char *)task_cb;

	string_list_append(&suite->failed, name);
	strbuf_addf(out, "FAILED TO START: '%s'\n", name);

	return 0;
}

static const char * const testsuite_usage[] = {
	"test-run-command testsuite [<options>] [<pattern>...]",
	NULL
};

static int testsuite(int argc, const char **argv)
{
	struct testsuite suite;
	int max_jobs = 1, i, ret;
	DIR *dir;
	struct dirent *d;
	struct option options[] = {
		OPT_BOOL('i', "immediate", &suite.immediate,
			 "stop at first failed test case(s)"),
		OPT_INTEGER('j', "jobs", &max_jobs, "run <N> jobs in parallel"),
		OPT_BOOL('q', "quiet", &suite.quiet, "be terse"),
		OPT_BOOL('v', "verbose", &suite.verbose, "be verbose"),
		OPT_BOOL('x', "trace", &suite.trace, "trace shell commands"),
		OPT_END()
	};

	memset(&suite, 0, sizeof(suite));
	suite.tests.strdup_strings = suite.failed.strdup_strings = 1;

	argc = parse_options(argc, argv, NULL, options,
			testsuite_usage, PARSE_OPT_STOP_AT_NON_OPTION);

	if (max_jobs <= 0)
		max_jobs = online_cpus();

	dir = opendir(".");
	if (!dir)
		die("Could not open the current directory");
	while ((d = readdir(dir))) {
		const char *p = d->d_name;

		if (*p != 't' || !isdigit(p[1]) || !isdigit(p[2]) ||
		    !isdigit(p[3]) || !isdigit(p[4]) || p[5] != '-' ||
		    !ends_with(p, ".sh"))
			continue;

		/* No pattern: match all */
		if (!argc) {
			string_list_append(&suite.tests, p);
			continue;
		}

		for (i = 0; i < argc; i++)
			if (!wildmatch(argv[i], p, 0)) {
				string_list_append(&suite.tests, p);
				break;
			}
	}
	closedir(dir);

	if (!suite.tests.nr)
		die("No tests match!");
	if (max_jobs > suite.tests.nr)
		max_jobs = suite.tests.nr;

	fprintf(stderr, "Running %d tests (%d at a time)\n",
		suite.tests.nr, max_jobs);

	ret = run_processes_parallel(max_jobs, next_test, test_failed,
				     test_finished, &suite);

	if (suite.failed.nr > 0) {
		ret = 1;
		fprintf(stderr, "%d tests failed:\n\n", suite.failed.nr);
		for (i = 0; i < suite.failed.nr; i++)
			fprintf(stderr, "\t%s\n", suite.failed.items[i].string);
	}

	string_list_clear(&suite.tests, 0);
	string_list_clear(&suite.failed, 0);

	return !!ret;
}

static int inherit_handle(const char *argv0)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	char path[PATH_MAX];
	int tmp;

	/* First, open an inheritable handle */
	sprintf(path, "out-XXXXXX");
	tmp = xmkstemp(path);

	argv_array_pushl(&cp.args, argv0, "inherited-handle-child", NULL);
	cp.in = -1;
	cp.no_stdout = cp.no_stderr = 1;
	if (start_command(&cp) < 0)
		die("Could not start child process");

	/* Then close it, and try to delete it. */
	close(tmp);
	if (unlink(path))
		die("Could not delete '%s'", path);

	if (close(cp.in) < 0 || finish_command(&cp) < 0)
		die("Child did not finish");

	return 0;
}

static int inherit_handle_child(void)
{
	struct strbuf buf = STRBUF_INIT;

	if (strbuf_read(&buf, 0, 0) < 0)
		die("Could not read stdin");
	printf("Received %s\n", buf.buf);
	strbuf_release(&buf);

	return 0;
}

int cmd_main(int argc, const char **argv)
{
	struct child_process proc = CHILD_PROCESS_INIT;
	int jobs;

	if (argc > 1 && !strcmp(argv[1], "testsuite"))
		exit(testsuite(argc - 1, argv + 1));

	if (!strcmp(argv[1], "inherited-handle"))
		exit(inherit_handle(argv[0]));
	if (!strcmp(argv[1], "inherited-handle-child"))
		exit(inherit_handle_child());

	if (argc < 3)
		return 1;
	while (!strcmp(argv[1], "env")) {
		if (!argv[2])
			die("env specifier without a value");
		argv_array_push(&proc.env_array, argv[2]);
		argv += 2;
		argc -= 2;
	}
	if (argc < 3)
		return 1;
	proc.argv = (const char **)argv + 2;

	if (!strcmp(argv[1], "start-command-ENOENT")) {
		if (start_command(&proc) < 0 && errno == ENOENT)
			return 0;
		fprintf(stderr, "FAIL %s\n", argv[1]);
		return 1;
	}
	if (!strcmp(argv[1], "run-command"))
		exit(run_command(&proc));

	jobs = atoi(argv[2]);
	proc.argv = (const char **)argv + 3;

	if (!strcmp(argv[1], "run-command-parallel"))
		exit(run_processes_parallel(jobs, parallel_next,
					    NULL, NULL, &proc));

	if (!strcmp(argv[1], "run-command-abort"))
		exit(run_processes_parallel(jobs, parallel_next,
					    NULL, task_finished, &proc));

	if (!strcmp(argv[1], "run-command-no-jobs"))
		exit(run_processes_parallel(jobs, no_job,
					    NULL, task_finished, &proc));

	fprintf(stderr, "check usage\n");
	return 1;
}
