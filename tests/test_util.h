/*
 * BENCsynth - the test harness, such as it is.
 *
 * Two counters and two assertions. A framework would be more code than the
 * tests it carries.
 */

#ifndef BS_TEST_UTIL_H
#define BS_TEST_UTIL_H

extern int bs_checks;
extern int bs_failures;

void ok(bool cond, const char *what);

/* For the ones worth printing the numbers of: `fmt` takes the value that was
 * measured and the value that was expected, in that order. */
void okf(bool cond, const char *fmt, double got, double want);

#endif /* BS_TEST_UTIL_H */
