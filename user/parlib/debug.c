#include <parlib/common.h>
#include <parlib/parlib.h>
#include <stdio.h>
#include <unistd.h>
#include <parlib/spinlock.h>
#include <ros/common.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

int akaros_printf(const char *format, ...)
{
	va_list ap;
	int ret;

	va_start(ap, format);
	ret = vprintf(format, ap);
	va_end(ap);
	return ret;
}

/* Poor man's Ftrace, won't work well with concurrency. */
static const char *blacklist[] = {
	"whatever",
};

static bool is_blacklisted(const char *s)
{
	for (int i = 0; i < COUNT_OF(blacklist); i++) {
		if (!strcmp(blacklist[i], s))
			return TRUE;
	}
	return FALSE;
}

static int tab_depth = 0;
static bool print = TRUE;

void reset_print_func_depth(void)
{
	tab_depth = 0;
}

void toggle_print_func(void)
{
	print = !print;
	printf("Func entry/exit printing is now %sabled\n", print ? "en" : "dis");
}

static spinlock_t lock = {0};

void __print_func_entry(const char *func, const char *file)
{
	if (!print)
		return;
	if (is_blacklisted(func))
		return;
	spinlock_lock(&lock);
	printd("Vcore %2d", vcore_id());	/* helps with multicore output */
	for (int i = 0; i < tab_depth; i++)
		printf("\t");
	printf("%s() in %s\n", func, file);
	spinlock_unlock(&lock);
	tab_depth++;
}

void __print_func_exit(const char *func, const char *file)
{
	if (!print)
		return;
	if (is_blacklisted(func))
		return;
	tab_depth--;
	spinlock_lock(&lock);
	printd("Vcore %2d", vcore_id());
	for (int i = 0; i < tab_depth; i++)
		printf("\t");
	printf("---- %s()\n", func);
	spinlock_unlock(&lock);
}

void trace_printf(const char *fmt, ...)
{
	static int kptrace;
	va_list args;
	char buf[128];
	int amt;

	run_once(
		kptrace = open("#kprof/kptrace", O_WRITE);
		if (kptrace < 0)
			perror("Unable to open kptrace!\n");
	);

	if (kptrace < 0)
		return;
	amt = snprintf(buf, sizeof(buf), "PID %d: ", getpid());
	/* amt could be > sizeof, if we truncated. */
	amt = MIN(amt, sizeof(buf));
	va_start(args, fmt);
	/* amt == sizeof is OK here */
	amt += vsnprintf(buf + amt, sizeof(buf) - amt, fmt, args);
	va_end(args);
	write(kptrace, buf, MIN(amt, sizeof(buf)));
}
