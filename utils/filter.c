#include <stdio.h>
#include <stdlib.h>
#include <regex.h>
#include <sys/utsname.h>
#include <link.h>

/* This should be defined before #include "utils.h" */
#define PR_FMT     "filter"
#define PR_DOMAIN  DBG_FILTER

#include "uftrace.h"
#include "libmcount/mcount.h"
#include "utils/filter.h"
#include "utils/symbol.h"
#include "utils/rbtree.h"
#include "utils/utils.h"
#include "utils/list.h"
#include "utils/auto-args.h"

/* RB-tree maintaining automatic arguments and return value */
static struct rb_root auto_argspec = RB_ROOT;
static struct rb_root auto_retspec = RB_ROOT;

static struct uftrace_filter * find_auto_args(struct rb_root *root, char *name);

static void snprintf_trigger_read(char *buf, size_t len,
				  enum trigger_read_type type)
{
	buf[0] = '\0';

	if (type == TRIGGER_READ_NONE)
		snprintf(buf, len, "none");

	if (type & TRIGGER_READ_PROC_STATM)
		snprintf(buf, len, "%s%s", buf[0] ? "|" : "", "proc/statm");
	if (type & TRIGGER_READ_PAGE_FAULT)
		snprintf(buf, len, "%s%s", buf[0] ? "|" : "", "page-fault");
}

static void print_trigger(struct uftrace_trigger *tr)
{
	if (tr->flags & TRIGGER_FL_DEPTH)
		pr_dbg("\ttrigger: depth %d\n", tr->depth);
	if (tr->flags & TRIGGER_FL_FILTER) {
		if (tr->fmode == FILTER_MODE_IN)
			pr_dbg("\ttrigger: filter IN\n");
		else
			pr_dbg("\ttrigger: filter OUT\n");
	}
	if (tr->flags & TRIGGER_FL_BACKTRACE)
		pr_dbg("\ttrigger: backtrace\n");
	if (tr->flags & TRIGGER_FL_TRACE)
		pr_dbg("\ttrigger: trace\n");
	if (tr->flags & TRIGGER_FL_TRACE_ON)
		pr_dbg("\ttrigger: trace_on\n");
	if (tr->flags & TRIGGER_FL_TRACE_OFF)
		pr_dbg("\ttrigger: trace_off\n");
	if (tr->flags & TRIGGER_FL_RECOVER)
		pr_dbg("\ttrigger: recover\n");
	if (tr->flags & TRIGGER_FL_FINISH)
		pr_dbg("\ttrigger: finish\n");

	if (tr->flags & TRIGGER_FL_ARGUMENT) {
		struct uftrace_arg_spec *arg;

		pr_dbg("\ttrigger: argument\n");
		list_for_each_entry(arg, tr->pargs, list) {
			if (arg->idx == RETVAL_IDX)
				continue;
			pr_dbg("\t\t arg%d: %c%d\n", arg->idx,
			       ARG_SPEC_CHARS[arg->fmt], arg->size * 8);
		}
	}
	if (tr->flags & TRIGGER_FL_RETVAL) {
		struct uftrace_arg_spec *arg;

		pr_dbg("\ttrigger: return value\n");
		list_for_each_entry(arg, tr->pargs, list) {
			if (arg->idx != RETVAL_IDX)
				continue;
			pr_dbg("\t\t retval%d: %c%d\n", arg->idx,
			       ARG_SPEC_CHARS[arg->fmt], arg->size * 8);
		}
	}

	if (tr->flags & TRIGGER_FL_COLOR)
		pr_dbg("\ttrigger: color '%c'\n", tr->color);
	if (tr->flags & TRIGGER_FL_TIME_FILTER)
		pr_dbg("\ttrigger: time filter %"PRIu64"\n", tr->time);

	if (tr->flags & TRIGGER_FL_READ) {
		char buf[1024];

		snprintf_trigger_read(buf, sizeof(buf), tr->read);
		pr_dbg("\ttrigger: read (%s)\n", buf);
	}
}

static bool match_ip(struct uftrace_filter *filter, unsigned long ip)
{
	return filter->start <= ip && ip < filter->end;
}

/**
 * uftrace_match_filter - try to match @ip with filters in @root
 * @ip   - instruction address to match
 * @root - root of rbtree which has filters
 * @tr   - trigger data
 */
struct uftrace_filter *uftrace_match_filter(uint64_t ip, struct rb_root *root,
					   struct uftrace_trigger *tr)
{
	struct rb_node *parent = NULL;
	struct rb_node **p = &root->rb_node;
	struct uftrace_filter *iter;

	while (*p) {
		parent = *p;
		iter = rb_entry(parent, struct uftrace_filter, node);

		if (match_ip(iter, ip)) {
			memcpy(tr, &iter->trigger, sizeof(*tr));

			pr_dbg2("filter match: %s\n", iter->name);
			if (dbg_domain[DBG_FILTER] >= 3)
				print_trigger(tr);
			return iter;
		}

		if (iter->start > ip)
			p = &parent->rb_left;
		else
			p = &parent->rb_right;
	}
	return NULL;
}

static void add_arg_spec(struct list_head *arg_list, struct uftrace_arg_spec *arg,
			 bool exact_match)
{
	bool found = false;
	struct uftrace_arg_spec *oarg, *narg;

	list_for_each_entry(oarg, arg_list, list) {
		switch (arg->type) {
		case ARG_TYPE_INDEX:
		case ARG_TYPE_FLOAT:
			if (arg->type == oarg->type && arg->idx == oarg->idx)
				found = true;
			break;
		case ARG_TYPE_REG:
			if (arg->reg_idx == oarg->reg_idx)
				found = true;
			break;
		case ARG_TYPE_STACK:
			if (arg->stack_ofs == oarg->stack_ofs)
				found = true;
			break;
		}

		if (found)
			break;
	}

	if (found) {
		/* do not overwrite exact match by regex match */
		if (exact_match || !oarg->exact) {
			oarg->fmt   = arg->fmt;
			oarg->size  = arg->size;
			oarg->exact = exact_match;
			oarg->type  = arg->type;
			oarg->reg_idx = arg->reg_idx;
		}
	}
	else {
		narg = xmalloc(sizeof(*narg));
		memcpy(narg, arg, sizeof(*narg));
		narg->exact = exact_match;

		/* sort args by index */
		list_for_each_entry(oarg, arg_list, list) {
			if (oarg->type == arg->type && oarg->idx > arg->idx)
				break;
		}

		list_add_tail(&narg->list, &oarg->list);
	}
}

static void add_trigger(struct uftrace_filter *filter, struct uftrace_trigger *tr,
			bool exact_match)
{
	filter->trigger.flags |= tr->flags;

	if (tr->flags & TRIGGER_FL_DEPTH)
		filter->trigger.depth = tr->depth;
	if (tr->flags & TRIGGER_FL_FILTER)
		filter->trigger.fmode = tr->fmode;

	if (tr->flags & TRIGGER_FL_TRACE_ON)
		filter->trigger.flags &= ~TRIGGER_FL_TRACE_OFF;
	if (tr->flags & TRIGGER_FL_TRACE_OFF)
		filter->trigger.flags &= ~TRIGGER_FL_TRACE_ON;

	if (tr->flags & (TRIGGER_FL_ARGUMENT | TRIGGER_FL_RETVAL)) {
		struct uftrace_arg_spec *arg;

		list_for_each_entry(arg, tr->pargs, list)
			add_arg_spec(&filter->args, arg, exact_match);
	}

	if (tr->flags & TRIGGER_FL_COLOR)
		filter->trigger.color = tr->color;
	if (tr->flags & TRIGGER_FL_TIME_FILTER)
		filter->trigger.time = tr->time;
	if (tr->flags & TRIGGER_FL_READ)
		filter->trigger.read = tr->read;
}

static int add_filter(struct rb_root *root, struct uftrace_filter *filter,
		       struct uftrace_trigger *tr, bool exact_match)
{
	struct rb_node *parent = NULL;
	struct rb_node **p = &root->rb_node;
	struct uftrace_filter *iter, *new;
	struct uftrace_filter *auto_arg = NULL;
	struct uftrace_filter *auto_ret = NULL;
	unsigned long orig_flags = tr->flags;  /* restored for regex filter */

	if ((tr->flags & TRIGGER_FL_ARGUMENT) && list_empty(tr->pargs)) {
		auto_arg = find_auto_args(&auto_argspec, filter->name);
		if (auto_arg == NULL)
			tr->flags &= ~TRIGGER_FL_ARGUMENT;
	}
	if ((tr->flags & TRIGGER_FL_RETVAL) && list_empty(tr->pargs)) {
		auto_arg = find_auto_args(&auto_retspec, filter->name);
		if (auto_arg == NULL)
			tr->flags &= ~TRIGGER_FL_RETVAL;
	}

	if (tr->flags == 0) {
		tr->flags = orig_flags;
		return 0;
	}

	pr_dbg("add filter for %s\n", filter->name);
	if (dbg_domain[DBG_FILTER] >= 3)
		print_trigger(tr);

	while (*p) {
		parent = *p;
		iter = rb_entry(parent, struct uftrace_filter, node);

		if (iter->start == filter->start) {
			add_trigger(iter, tr, exact_match);
			if (auto_arg)
				add_trigger(iter, &auto_arg->trigger, exact_match);
			if (auto_ret)
				add_trigger(iter, &auto_ret->trigger, exact_match);
			tr->flags = orig_flags;
			return 1;
		}

		if (iter->start > filter->start)
			p = &parent->rb_left;
		else
			p = &parent->rb_right;
	}

	new = xmalloc(sizeof(*new));
	memcpy(new, filter, sizeof(*new));
	new->trigger.flags = 0;
	INIT_LIST_HEAD(&new->args);
	new->trigger.pargs = &new->args;

	add_trigger(new, tr, exact_match);
	if (auto_arg)
		add_trigger(new, &auto_arg->trigger, exact_match);
	if (auto_ret)
		add_trigger(new, &auto_ret->trigger, exact_match);
	tr->flags = orig_flags;

	rb_link_node(&new->node, parent, p);
	rb_insert_color(&new->node, root);
	return 1;
}

static int add_exact_filter(struct rb_root *root, struct symtab *symtab,
			    char *filter_str, struct uftrace_trigger *tr)
{
	struct uftrace_filter filter;
	struct sym *sym;

	sym = find_symname(symtab, filter_str);
	if (sym == NULL)
		return 0;

	filter.name = sym->name;
	filter.start = sym->addr;
	filter.end = sym->addr + sym->size;

	return add_filter(root, &filter, tr, true);
}

static int add_regex_filter(struct rb_root *root, struct symtab *symtab,
			    char *filter_str, struct uftrace_trigger *tr)
{
	struct uftrace_filter filter;
	struct sym *sym;
	regex_t re;
	unsigned i;
	int ret = 0;

	if (regcomp(&re, filter_str, REG_NOSUB | REG_EXTENDED)) {
		pr_dbg("regex pattern failed: %s\n", filter_str);
		return 0;
	}

	for (i = 0; i < symtab->nr_sym; i++) {
		sym = &symtab->sym[i];

		if (regexec(&re, sym->name, 0, NULL, 0))
			continue;

		filter.name = sym->name;
		filter.start = sym->addr;
		filter.end = sym->addr + sym->size;

		ret += add_filter(root, &filter, tr, false);
	}

	regfree(&re);
	return ret;
}

static bool is_arm_machine(void)
{
	static char *mach = NULL;

	if (mach == NULL) {
		struct utsname utsbuf;

		uname(&utsbuf);
		mach = xstrdup(utsbuf.machine);
	}

	return mach[0] == 'a' && mach[1] == 'r' && mach[2] == 'm';
}

static int check_so_cb(struct dl_phdr_info *info, size_t size, void *data)
{
	const char *soname = data;
	int so_used = 0;

	if (!strncmp(basename(info->dlpi_name), soname, strlen(soname)))
		so_used = 1;

	return so_used;
}

/* check whether the given library name is in shared object list */
static int has_shared_object(const char *soname)
{
	static int so_used = -1;

	if (so_used != -1)
		return so_used;

	so_used = dl_iterate_phdr(check_so_cb, (void*)soname);

	return so_used;
}

/* argument_spec = arg1/i32,arg2/x64,... */
static int parse_spec(char *str, struct uftrace_arg_spec *arg, char *suffix)
{
	int fmt = ARG_FMT_AUTO;
	int size = sizeof(long);
	int type = arg->type;
	int bit;

	if (suffix == NULL || *suffix == '\0')
		goto out;

	if (*suffix == '%')
		goto type;

	suffix++;
	switch (*suffix) {
	case 'i':
		fmt = ARG_FMT_SINT;
		break;
	case 'u':
		fmt = ARG_FMT_UINT;
		break;
	case 'x':
		fmt = ARG_FMT_HEX;
		break;
	case 's':
		fmt = ARG_FMT_STR;
		break;
	case 'c':
		fmt = ARG_FMT_CHAR;
		size = sizeof(char);
		break;
	case 'f':
		fmt = ARG_FMT_FLOAT;
		type = ARG_TYPE_FLOAT;
		size = sizeof(double);
		break;
	case 'S':
		if (has_shared_object("libc++.so")) {
			static bool warned = false;
			if (!warned) {
				pr_warn("std::string display for libc++.so is "
					"not supported.\n");
				warned = true;
			}
			return -1;
		}
		fmt = ARG_FMT_STD_STRING;
		break;
	default:
		pr_use("unsupported argument type: %s\n", str);
		return -1;
	}

	suffix++;
	if (*suffix == '\0')
		goto out;
	if (*suffix == '%')
		goto type;

	bit = strtol(suffix, &suffix, 10);
	switch (bit) {
	case 8:
	case 16:
	case 32:
	case 64:
		size = bit / 8;
		break;
	case 80:
		if (fmt == ARG_FMT_FLOAT) {
			size = bit / 8;
			break;
		}
		/* fall through */
	default:
		pr_use("unsupported argument size: %s\n", str);
		return -1;
	}

type:
	if (*suffix == '%') {
		suffix++;

		if (!strncmp(suffix, "stack", 5)) {
			arg->stack_ofs = strtol(suffix+5, NULL, 0);
			type = ARG_TYPE_STACK;
		}
		else {
			arg->reg_idx = arch_register_index(suffix);
			type = ARG_TYPE_REG;

			if (arg->reg_idx < 0) {
				pr_use("unknown register name: %s\n", str);
				return -1;
			}
		}
	}

out:
	/* it seems ARM falls back 'long double' to 'double' */
	if (fmt == ARG_FMT_FLOAT && size == 10 && is_arm_machine())
		size = 8;

	arg->fmt  = fmt;
	arg->size = size;
	arg->type = type;

	return 0;
}

/* argument_spec = arg1/i32,arg2/x64%reg,arg3%stack+1,... */
static int parse_argument_spec(char *str, struct uftrace_trigger *tr)
{
	struct uftrace_arg_spec *arg;
	char *suffix;

	if (!isdigit(str[3])) {
		pr_use("skipping invalid argument: %s\n", str);
		return -1;
	}

	arg = xmalloc(sizeof(*arg));
	INIT_LIST_HEAD(&arg->list);
	arg->idx = strtol(str+3, &suffix, 0);
	arg->type = ARG_TYPE_INDEX;

	if (parse_spec(str, arg, suffix) == -1) {
		free(arg);
		return -1;
	}

	tr->flags |= TRIGGER_FL_ARGUMENT;
	list_add_tail(&arg->list, tr->pargs);

	return 0;
}
/* argument_spec = retval/i32 or retval/x64 ... */
static int parse_retval_spec(char *str, struct uftrace_trigger *tr)
{
	struct uftrace_arg_spec *arg;
	char *suffix;

	arg = xmalloc(sizeof(*arg));
	INIT_LIST_HEAD(&arg->list);
	arg->idx = 0;
	arg->type = ARG_TYPE_INDEX;

	/* set suffix after string "retval" */
	suffix = str + 6;

	if (parse_spec(str, arg, suffix) == -1) {
		free(arg);
		return -1;
	}

	tr->flags |= TRIGGER_FL_RETVAL;
	list_add_tail(&arg->list, tr->pargs);

	return 0;
}

/* argument_spec = fparg1/32,fparg2/64%stack+1,... */
static int parse_float_argument_spec(char *str, struct uftrace_trigger *tr)
{
	struct uftrace_arg_spec *arg;
	char *suffix;

	if (!isdigit(str[5])) {
		pr_use("skipping invalid argument: %s\n", str);
		return -1;
	}

	arg = xmalloc(sizeof(*arg));
	INIT_LIST_HEAD(&arg->list);
	arg->idx = strtol(str+5, &suffix, 0);
	arg->fmt = ARG_FMT_FLOAT;
	arg->type = ARG_TYPE_FLOAT;
	arg->size = sizeof(double);

	if (*suffix == '/') {
		long size = strtol(suffix+1, &suffix, 0);

		if (size != 32 && size != 64 && size != 80) {
			pr_use("invalid argument size: %s\n", str);
			free(arg);
			return -1;
		}
		if (size == 80 && is_arm_machine())
			size = 64;

		arg->size = size / 8;
	}

	if (*suffix == '%') {
		suffix++;

		if (!strncmp(suffix, "stack", 5)) {
			arg->stack_ofs = strtol(suffix+5, NULL, 0);
			arg->type = ARG_TYPE_STACK;
		}
		else {
			arg->reg_idx = arch_register_index(suffix);
			arg->type = ARG_TYPE_REG;

			if (arg->reg_idx < 0) {
				pr_use("unknown register name: %s\n", str);
				free(arg);
				return -1;
			}
		}
	}

	tr->flags |= TRIGGER_FL_ARGUMENT;
	list_add_tail(&arg->list, tr->pargs);

	return 0;
}

static int parse_depth_action(char *action, struct uftrace_trigger *tr)
{
	tr->flags |= TRIGGER_FL_DEPTH;
	tr->depth = strtoul(action + 6, NULL, 10);

	if (tr->depth < 0 || tr->depth > MCOUNT_RSTACK_MAX) {
		pr_use("skipping invalid trigger depth: %d\n", tr->depth);
		return -1;
	}
	return 0;
}

static int parse_time_action(char *action, struct uftrace_trigger *tr)
{
	tr->flags |= TRIGGER_FL_TIME_FILTER;
	tr->time = parse_time(action + 5, 3);
	return 0;
}

static int parse_read_action(char *action, struct uftrace_trigger *tr)
{
	const char *target = action + 5;

	if (!strcmp(target, "proc/statm"))
		tr->read |= TRIGGER_READ_PROC_STATM;
	if (!strcmp(target, "page-fault"))
		tr->read |= TRIGGER_READ_PAGE_FAULT;

	/* set READ flag only if valid type set */
	if (tr->read)
		tr->flags |= TRIGGER_FL_READ;

	return 0;
}

static int parse_color_action(char *action, struct uftrace_trigger *tr)
{
	const char *color = action + 6;

	if (!strcmp(color, "red"))
		tr->color = COLOR_CODE_RED;
	else if (!strcmp(color, "green"))
		tr->color = COLOR_CODE_GREEN;
	else if (!strcmp(color, "blue"))
		tr->color = COLOR_CODE_BLUE;
	else if (!strcmp(color, "yellow"))
		tr->color = COLOR_CODE_YELLOW;
	else if (!strcmp(color, "magenta"))
		tr->color = COLOR_CODE_MAGENTA;
	else if (!strcmp(color, "cyan"))
		tr->color = COLOR_CODE_CYAN;
	else if (!strcmp(color, "bold"))
		tr->color = COLOR_CODE_BOLD;
	else if (!strcmp(color, "gray"))
		tr->color = COLOR_CODE_GRAY;
	else {
		pr_use("ignoring invalid color: %s\n", color);
		return 0;
	}

	tr->flags |= TRIGGER_FL_COLOR;
	return 0;
}

static int parse_trace_action(char *action, struct uftrace_trigger *tr)
{
	action += 5;
	if (*action == '_' || *action == '-')
		action++;

	if (*action == '\0')
		tr->flags |= TRIGGER_FL_TRACE;
	else if (!strcasecmp(action, "on"))
		tr->flags |= TRIGGER_FL_TRACE_ON;
	else if (!strcasecmp(action, "off"))
		tr->flags |= TRIGGER_FL_TRACE_OFF;
	else
		pr_use("skipping invalid trace action: %s\n", action);

	return 0;
}

static int parse_backtrace_action(char *action, struct uftrace_trigger *tr)
{
	tr->flags |= TRIGGER_FL_BACKTRACE;
	return 0;
}

static int parse_recover_action(char *action, struct uftrace_trigger *tr)
{
	tr->flags |= TRIGGER_FL_RECOVER;
	return 0;
}

static int parse_finish_action(char *action, struct uftrace_trigger *tr)
{
	tr->flags |= TRIGGER_FL_FINISH;
	return 0;
}

static int parse_filter_action(char *action, struct uftrace_trigger *tr)
{
	tr->flags |= TRIGGER_FL_FILTER;
	tr->fmode  = FILTER_MODE_IN;
	return 0;
}

static int parse_notrace_action(char *action, struct uftrace_trigger *tr)
{
	tr->flags |= TRIGGER_FL_FILTER;
	tr->fmode  = FILTER_MODE_OUT;
	return 0;
}

struct trigger_action_parser {
	const char *name;
	int (*parse)(char *action, struct uftrace_trigger *tr);
	unsigned long flags;
};

static const struct trigger_action_parser actions[] = {
	{ "arg",       parse_argument_spec,       TRIGGER_FL_ARGUMENT, },
	{ "fparg",     parse_float_argument_spec, TRIGGER_FL_ARGUMENT, },
	{ "retval",    parse_retval_spec,         TRIGGER_FL_RETVAL, },
	{ "filter",    parse_filter_action,       TRIGGER_FL_FILTER, },
	{ "notrace",   parse_notrace_action,      TRIGGER_FL_FILTER, },
	{ "depth=",    parse_depth_action,        TRIGGER_FL_FILTER, },
	{ "time=",     parse_time_action,         TRIGGER_FL_FILTER, },
	{ "read=",     parse_read_action, },
	{ "color=",    parse_color_action, },
	{ "trace",     parse_trace_action, },
	{ "backtrace", parse_backtrace_action, },
	{ "recover",   parse_recover_action, },
	{ "finish",    parse_finish_action, },
};

static int setup_trigger_action(char *str, struct uftrace_trigger *tr,
				char **module, unsigned long orig_flags)
{
	char *tr_str, *tmp;
	char *pos = strchr(str, '@');
	int ret = -1;
	size_t i;

	if (pos == NULL)
		return 0;

	*pos++ = '\0';
	tmp = tr_str = xstrdup(pos);

	while ((pos = strsep(&tmp, ",")) != NULL) {
		for (i = 0; i < ARRAY_SIZE(actions); i++) {
			const struct trigger_action_parser *action = &actions[i];

			if (strncasecmp(pos, action->name, strlen(action->name)))
				continue;

			if (orig_flags && !(orig_flags & action->flags))
				break;  /* ignore incompatible actions */

			if (action->parse(pos, tr) < 0)
				goto out;

			break;
		}

		/* if it's not an action, treat it as a module name */
		if (i == ARRAY_SIZE(actions)) {
			if (*module)
				pr_use("ignoring extra module: %s\n", pos);
			else
				*module = xstrdup(pos);
		}
	}
	ret = 0;

out:
	free(tr_str);
	return ret;
}

static int add_trigger_entry(struct rb_root *root, struct symtab *symtab,
			     char *name, bool is_regex,
			     struct uftrace_trigger *tr)
{
	if (is_regex)
		return add_regex_filter(root, symtab, name, tr);
	else
		return add_exact_filter(root, symtab, name, tr);
}

static void setup_trigger(char *filter_str, struct symtabs *symtabs,
			  struct rb_root *root,
			  unsigned long flags, enum filter_mode *fmode)
{
	char *str;
	char *pos, *name;

	if (filter_str == NULL)
		return;

	pos = str = strdup(filter_str);
	if (str == NULL)
		return;

	name = strtok(pos, ";");
	while (name) {
		LIST_HEAD(args);
		struct uftrace_trigger tr = {
			.flags = flags,
			.pargs = &args,
		};
		int ret = 0;
		char *module = NULL;
		struct uftrace_arg_spec *arg;
		struct uftrace_mmap *map;
		bool is_regex;

		if (setup_trigger_action(name, &tr, &module, flags) < 0)
			goto next;

		/* skip unintended kernel symbols */
		if (module && !strcasecmp(module, "kernel"))
			goto next;

		if (flags & TRIGGER_FL_FILTER) {
			if (name[0] == '!') {
				tr.fmode = FILTER_MODE_OUT;
				name++;
			}
			else
				tr.fmode = FILTER_MODE_IN;
		}

		is_regex = strpbrk(name, REGEX_CHARS);

		if (module) {
			map = find_map_by_name(symtabs, module);
			if (map == NULL && strcasecmp(module, "PLT")) {
				free(module);
				goto next;
			}

			/* is it the main executable? */
			if (!strncmp(module, basename(symtabs->filename),
				     strlen(module))) {
				ret += add_trigger_entry(root, &symtabs->symtab,
							 name, is_regex, &tr);
				ret += add_trigger_entry(root, &symtabs->dsymtab,
							 name, is_regex, &tr);
			}
			else if (!strcasecmp(module, "PLT")) {
				ret = add_trigger_entry(root, &symtabs->dsymtab,
							name, is_regex, &tr);
			}
			else {
				ret = add_trigger_entry(root, &map->symtab,
							name, is_regex, &tr);
			}

			free(module);
		}
		else {
			/* check main executable's symtab first */
			ret += add_trigger_entry(root, &symtabs->symtab, name,
						 is_regex, &tr);
			ret += add_trigger_entry(root, &symtabs->dsymtab, name,
						 is_regex, &tr);

			/* and then find all module's symtabs */
			map = symtabs->maps;
			while (map) {
				ret += add_trigger_entry(root, &map->symtab,
							 name, is_regex, &tr);
				map = map->next;
			}
		}

		if (ret > 0 && (tr.flags & TRIGGER_FL_FILTER) && fmode) {
			if (tr.fmode == FILTER_MODE_IN)
				*fmode = FILTER_MODE_IN;
			else if (*fmode == FILTER_MODE_NONE)
				*fmode = FILTER_MODE_OUT;
		}
next:
		name = strtok(NULL, ";");

		while (!list_empty(&args)) {
			arg = list_first_entry(&args, struct uftrace_arg_spec, list);
			list_del(&arg->list);
			free(arg);
		}

	}

	free(str);
}

static void add_auto_args(struct rb_root *root, struct uftrace_filter *entry,
			  struct uftrace_trigger *tr)
{
	struct rb_node *parent = NULL;
	struct rb_node **p = &root->rb_node;
	struct uftrace_filter *iter, *new;
	int cmp;

	pr_dbg2("add auto-argument for %s\n", entry->name);
	if (dbg_domain[DBG_FILTER] >= 3)
		print_trigger(tr);

	while (*p) {
		parent = *p;
		iter = rb_entry(parent, struct uftrace_filter, node);

		cmp = strcmp(iter->name, entry->name);
		if (cmp == 0) {
			add_trigger(iter, tr, true);
			return;
		}

		if (cmp < 0)
			p = &parent->rb_left;
		else
			p = &parent->rb_right;
	}

	new = xmalloc(sizeof(*new));
	memcpy(new, entry, sizeof(*new));
	new->trigger.flags = 0;
	INIT_LIST_HEAD(&new->args);
	new->trigger.pargs = &new->args;

	add_trigger(new, tr, true);

	rb_link_node(&new->node, parent, p);
	rb_insert_color(&new->node, root);
}

static struct uftrace_filter * find_auto_args(struct rb_root *root, char *name)
{
	struct rb_node *parent = NULL;
	struct rb_node **p = &root->rb_node;
	struct uftrace_filter *iter;
	int cmp;

	while (*p) {
		parent = *p;
		iter = rb_entry(parent, struct uftrace_filter, node);

		cmp = strcmp(iter->name, name);
		if (cmp == 0)
			return iter;

		if (cmp < 0)
			p = &parent->rb_left;
		else
			p = &parent->rb_right;
	}

	return NULL;
}

static void setup_auto_args(const char *args_str, struct rb_root *root,
			    unsigned long flag)
{
	char *str;
	char *pos, *name;

	if (args_str == NULL)
		return;

	pos = str = strdup(args_str);
	if (str == NULL)
		return;

	name = strtok(pos, ";");
	while (name) {
		LIST_HEAD(args);
		struct uftrace_arg_spec *arg;
		struct uftrace_trigger tr = {
			.pargs = &args,
		};
		struct uftrace_filter entry = {
			.name = NULL,
		};

		if (setup_trigger_action(name, &tr, NULL, flag) < 0)
			goto next;

		/*
		 * it should be copied after setup_trigger_action() removed
		 * '@' for the arg spec
		 */
		entry.name = xstrdup(name);
		add_auto_args(root, &entry, &tr);
next:
		name = strtok(NULL, ";");

		while (!list_empty(&args)) {
			arg = list_first_entry(&args, struct uftrace_arg_spec, list);
			list_del(&arg->list);
			free(arg);
		}
	}

	free(str);
}

static void finish_auto_args(struct rb_root *root)
{
	struct rb_node *p;
	struct uftrace_filter *entry;
	struct uftrace_arg_spec *arg, *tmp;

	while (!RB_EMPTY_ROOT(root)) {
		p = rb_first(root);
		entry = rb_entry(p, struct uftrace_filter, node);

		rb_erase(p, root);

		list_for_each_entry_safe(arg, tmp, &entry->args, list) {
			list_del(&arg->list);
			free(arg);
		}

		free(entry->name);
		free(entry);
	}
}
/**
 * uftrace_setup_filter - construct rbtree of filters
 * @filter_str - CSV of filter string
 * @symtabs    - symbol tables to find symbol address
 * @root       - root of resulting rbtree
 * @mode       - filter mode: opt-in (-F) or opt-out (-N)
 */
void uftrace_setup_filter(char *filter_str, struct symtabs *symtabs,
			  struct rb_root *root, enum filter_mode *mode)
{
	setup_trigger(filter_str, symtabs, root, TRIGGER_FL_FILTER, mode);
}

/**
 * uftrace_setup_trigger - construct rbtree of triggers
 * @trigger_str - CSV of trigger string (FUNC @ act)
 * @symtabs    - symbol tables to find symbol address
 * @root       - root of resulting rbtree
 */
void uftrace_setup_trigger(char *trigger_str, struct symtabs *symtabs,
			   struct rb_root *root, enum filter_mode *mode)
{
	setup_trigger(trigger_str, symtabs, root, 0, mode);
}

/**
 * uftrace_setup_argument - construct rbtree of argument
 * @args_str   - CSV of argument string (FUNC @ arg)
 * @symtabs    - symbol tables to find symbol address
 * @root       - root of resulting rbtree
 */
void uftrace_setup_argument(char *args_str, struct symtabs *symtabs,
			    struct rb_root *root)
{
	setup_auto_args(auto_args_list, &auto_argspec, TRIGGER_FL_ARGUMENT);
	setup_trigger(args_str, symtabs, root, TRIGGER_FL_ARGUMENT, NULL);
}

/**
 * uftrace_setup_retval - construct rbtree of retval
 * @retval_str   - CSV of argument string (FUNC @ arg)
 * @symtabs    - symbol tables to find symbol address
 * @root       - root of resulting rbtree
 */
void uftrace_setup_retval(char *retval_str, struct symtabs *symtabs,
			  struct rb_root *root)
{
	setup_auto_args(auto_retvals_list, &auto_retspec, TRIGGER_FL_RETVAL);
	setup_trigger(retval_str, symtabs, root, TRIGGER_FL_RETVAL, NULL);
}

/**
 * uftrace_cleanup_filter - delete filters in rbtree
 * @root - root of the filter rbtree
 */
void uftrace_cleanup_filter(struct rb_root *root)
{
	struct rb_node *node;
	struct uftrace_filter *filter;
	struct uftrace_arg_spec *arg, *tmp;

	while (!RB_EMPTY_ROOT(root)) {
		node = rb_first(root);
		filter = rb_entry(node, struct uftrace_filter, node);

		rb_erase(node, root);

		list_for_each_entry_safe(arg, tmp, &filter->args, list) {
			list_del(&arg->list);
			free(arg);
		}
		free(filter);
	}

	finish_auto_args(&auto_argspec);
	finish_auto_args(&auto_retspec);
}

/**
 * uftrace_print_filter - print all filters in rbtree
 * @root - root of the filter rbtree
 */
void uftrace_print_filter(struct rb_root *root)
{
	struct rb_node *node;
	struct uftrace_filter *filter;

	node = rb_first(root);
	while (node) {
		filter = rb_entry(node, struct uftrace_filter, node);
		pr_dbg("%lx-%lx: %s\n", filter->start, filter->end, filter->name);
		print_trigger(&filter->trigger);

		node = rb_next(node);
	}
}

char * uftrace_clear_kernel(char *filter_str)
{
	char *str, *pos, *ret, *tmp;

	/* check filter string contains a kernel filter */
	if (filter_str == NULL)
		return NULL;

	if (strstr(filter_str, "@kernel") == NULL)
		return xstrdup(filter_str);

	str = pos = xstrdup(filter_str);
	ret = NULL;

	pos = strtok_r(pos, ";", &tmp);
	while (pos) {
		if (strstr(pos, "@kernel") == NULL)
			ret = strjoin(ret, pos, ";");

		pos = strtok_r(NULL, ";", &tmp);
	}
	free(str);

	return ret;
}

#ifdef UNIT_TEST

static void filter_test_load_symtabs(struct symtabs *stabs)
{
	static struct sym syms[] = {
		{ 0x1000, 0x1000, ST_GLOBAL, "foo::foo" },
		{ 0x2000, 0x1000, ST_GLOBAL, "foo::bar" },
		{ 0x3000, 0x1000, ST_GLOBAL, "foo::baz1" },
		{ 0x4000, 0x1000, ST_GLOBAL, "foo::baz2" },
		{ 0x5000, 0x1000, ST_GLOBAL, "foo::baz3" },
		{ 0x6000, 0x1000, ST_GLOBAL, "foo::~foo" },
	};
	static struct sym dsyms[] = {
		{ 0x21000, 0x1000, ST_PLT, "malloc" },
		{ 0x22000, 0x1000, ST_PLT, "free" },
	};

	stabs->symtab.sym = syms;
	stabs->symtab.nr_sym = ARRAY_SIZE(syms);
	stabs->dsymtab.sym = dsyms;
	stabs->dsymtab.nr_sym = ARRAY_SIZE(dsyms);
	stabs->loaded = true;
}

TEST_CASE(filter_setup_exact)
{
	struct symtabs stabs = {
		.loaded = false,
	};
	struct rb_root root = RB_ROOT;
	struct rb_node *node;
	struct uftrace_filter *filter;

	filter_test_load_symtabs(&stabs);

	/* test1: simple method */
	uftrace_setup_filter("foo::bar", &stabs, &root, NULL);
	TEST_EQ(RB_EMPTY_ROOT(&root), false);

	node = rb_first(&root);
	filter = rb_entry(node, struct uftrace_filter, node);
	TEST_STREQ(filter->name, "foo::bar");
	TEST_EQ(filter->start, 0x2000UL);
	TEST_EQ(filter->end, 0x2000UL + 0x1000UL);

	uftrace_cleanup_filter(&root);
	TEST_EQ(RB_EMPTY_ROOT(&root), true);

	/* test2: destructor */
	uftrace_setup_filter("foo::~foo", &stabs, &root, NULL);
	TEST_EQ(RB_EMPTY_ROOT(&root), false);

	node = rb_first(&root);
	filter = rb_entry(node, struct uftrace_filter, node);
	TEST_STREQ(filter->name, "foo::~foo");
	TEST_EQ(filter->start, 0x6000UL);
	TEST_EQ(filter->end, 0x6000UL + 0x1000UL);

	uftrace_cleanup_filter(&root);
	TEST_EQ(RB_EMPTY_ROOT(&root), true);

	/* test3: unknown symbol */
	uftrace_setup_filter("invalid_name", &stabs, &root, NULL);
	TEST_EQ(RB_EMPTY_ROOT(&root), true);

	return TEST_OK;
}

TEST_CASE(filter_setup_regex)
{
	struct symtabs stabs = {
		.loaded = false,
	};;
	struct rb_root root = RB_ROOT;
	struct rb_node *node;
	struct uftrace_filter *filter;

	filter_test_load_symtabs(&stabs);

	uftrace_setup_filter("foo::b.*", &stabs, &root, NULL);
	TEST_EQ(RB_EMPTY_ROOT(&root), false);

	node = rb_first(&root);
	filter = rb_entry(node, struct uftrace_filter, node);
	TEST_STREQ(filter->name, "foo::bar");
	TEST_EQ(filter->start, 0x2000UL);
	TEST_EQ(filter->end, 0x2000UL + 0x1000UL);

	node = rb_next(node);
	filter = rb_entry(node, struct uftrace_filter, node);
	TEST_STREQ(filter->name, "foo::baz1");
	TEST_EQ(filter->start, 0x3000UL);
	TEST_EQ(filter->end, 0x3000UL + 0x1000UL);

	node = rb_next(node);
	filter = rb_entry(node, struct uftrace_filter, node);
	TEST_STREQ(filter->name, "foo::baz2");
	TEST_EQ(filter->start, 0x4000UL);
	TEST_EQ(filter->end, 0x4000UL + 0x1000UL);

	node = rb_next(node);
	filter = rb_entry(node, struct uftrace_filter, node);
	TEST_STREQ(filter->name, "foo::baz3");
	TEST_EQ(filter->start, 0x5000UL);
	TEST_EQ(filter->end, 0x5000UL + 0x1000UL);

	uftrace_cleanup_filter(&root);
	TEST_EQ(RB_EMPTY_ROOT(&root), true);

	return TEST_OK;
}

TEST_CASE(filter_setup_notrace)
{
	struct symtabs stabs = {
		.loaded = false,
	};;
	struct rb_root root = RB_ROOT;
	struct rb_node *node;
	struct uftrace_filter *filter;
	enum filter_mode fmode;

	filter_test_load_symtabs(&stabs);

	uftrace_setup_filter("foo::.*", &stabs, &root, &fmode);
	TEST_EQ(RB_EMPTY_ROOT(&root), false);
	TEST_EQ(fmode, FILTER_MODE_IN);

	uftrace_setup_filter("!foo::foo", &stabs, &root, &fmode);
	TEST_EQ(RB_EMPTY_ROOT(&root), false);
	TEST_EQ(fmode, FILTER_MODE_IN);  /* overall filter mode doesn't change */

	node = rb_first(&root);
	filter = rb_entry(node, struct uftrace_filter, node);
	TEST_STREQ(filter->name, "foo::foo");
	TEST_EQ(filter->trigger.flags, TRIGGER_FL_FILTER);
	TEST_EQ(filter->trigger.fmode, FILTER_MODE_OUT);

	node = rb_next(node);
	filter = rb_entry(node, struct uftrace_filter, node);
	TEST_STREQ(filter->name, "foo::bar");
	TEST_EQ(filter->trigger.flags, TRIGGER_FL_FILTER);
	TEST_EQ(filter->trigger.fmode, FILTER_MODE_IN);

	uftrace_cleanup_filter(&root);
	TEST_EQ(RB_EMPTY_ROOT(&root), true);

	return TEST_OK;
}

TEST_CASE(filter_match)
{
	struct symtabs stabs = {
		.loaded = false,
	};;
	struct rb_root root = RB_ROOT;
	struct rb_node *node;
	struct uftrace_filter *filter;
	enum filter_mode fmode;
	struct uftrace_trigger tr;

	filter_test_load_symtabs(&stabs);

	uftrace_setup_filter("foo::foo", &stabs, &root, &fmode);
	TEST_EQ(RB_EMPTY_ROOT(&root), false);
	TEST_EQ(fmode, FILTER_MODE_IN);

	memset(&tr, 0, sizeof(tr));
	TEST_NE(uftrace_match_filter(0x1000, &root, &tr), NULL);
	TEST_EQ(tr.flags, TRIGGER_FL_FILTER);
	TEST_EQ(tr.fmode, FILTER_MODE_IN);

	memset(&tr, 0, sizeof(tr));
	TEST_NE(uftrace_match_filter(0x1fff, &root, &tr), NULL);
	TEST_EQ(tr.flags, TRIGGER_FL_FILTER);
	TEST_EQ(tr.fmode, FILTER_MODE_IN);

	memset(&tr, 0, sizeof(tr));
	TEST_EQ(uftrace_match_filter(0xfff, &root, &tr), NULL);
	TEST_NE(tr.flags, TRIGGER_FL_FILTER);

	memset(&tr, 0, sizeof(tr));
	TEST_EQ(uftrace_match_filter(0x2000, &root, &tr), NULL);
	TEST_NE(tr.flags, TRIGGER_FL_FILTER);

	uftrace_cleanup_filter(&root);
	TEST_EQ(RB_EMPTY_ROOT(&root), true);

	return TEST_OK;
}

TEST_CASE(trigger_setup_actions)
{
	struct symtabs stabs = {
		.loaded = false,
	};;
	struct rb_root root = RB_ROOT;
	struct rb_node *node;
	struct uftrace_filter *filter;
	struct uftrace_trigger tr;

	filter_test_load_symtabs(&stabs);

	uftrace_setup_trigger("foo::bar@depth=2", &stabs, &root, NULL);
	TEST_EQ(RB_EMPTY_ROOT(&root), false);

	memset(&tr, 0, sizeof(tr));
	TEST_NE(uftrace_match_filter(0x2500, &root, &tr), NULL);
	TEST_EQ(tr.flags, TRIGGER_FL_DEPTH);
	TEST_EQ(tr.depth, 2);

	uftrace_setup_trigger("foo::bar@backtrace", &stabs, &root, NULL);
	memset(&tr, 0, sizeof(tr));
	TEST_NE(uftrace_match_filter(0x2500, &root, &tr), NULL);
	TEST_EQ(tr.flags, TRIGGER_FL_DEPTH | TRIGGER_FL_BACKTRACE);

	uftrace_setup_trigger("foo::baz1@traceon", &stabs, &root, NULL);
	memset(&tr, 0, sizeof(tr));
	TEST_NE(uftrace_match_filter(0x3000, &root, &tr), NULL);
	TEST_EQ(tr.flags, TRIGGER_FL_TRACE_ON);

	uftrace_setup_trigger("foo::baz3@trace_off,depth=1", &stabs, &root, NULL);
	memset(&tr, 0, sizeof(tr));
	TEST_NE(uftrace_match_filter(0x5000, &root, &tr), NULL);
	TEST_EQ(tr.flags, TRIGGER_FL_TRACE_OFF | TRIGGER_FL_DEPTH);
	TEST_EQ(tr.depth, 1);

	uftrace_cleanup_filter(&root);
	TEST_EQ(RB_EMPTY_ROOT(&root), true);

	return TEST_OK;
}

TEST_CASE(trigger_setup_filters)
{
	struct symtabs stabs = {
		.loaded = false,
	};;
	struct rb_root root = RB_ROOT;
	struct rb_node *node;
	struct uftrace_filter *filter;
	struct uftrace_trigger tr;
	enum filter_mode fmode;

	filter_test_load_symtabs(&stabs);

	uftrace_setup_trigger("foo::bar@depth=2,notrace", &stabs, &root, &fmode);
	TEST_EQ(RB_EMPTY_ROOT(&root), false);
	TEST_EQ(fmode, FILTER_MODE_OUT);

	memset(&tr, 0, sizeof(tr));
	TEST_NE(uftrace_match_filter(0x2500, &root, &tr), NULL);
	TEST_EQ(tr.flags, TRIGGER_FL_DEPTH | TRIGGER_FL_FILTER);
	TEST_EQ(tr.depth, 2);
	TEST_EQ(tr.fmode, FILTER_MODE_OUT);

	uftrace_setup_filter("foo::baz1", &stabs, &root, &fmode);
	TEST_EQ(fmode, FILTER_MODE_IN);

	memset(&tr, 0, sizeof(tr));
	TEST_NE(uftrace_match_filter(0x3000, &root, &tr), NULL);
	TEST_EQ(tr.flags, TRIGGER_FL_FILTER);
	TEST_EQ(tr.fmode, FILTER_MODE_IN);

	uftrace_setup_trigger("foo::baz2@notrace", &stabs, &root, &fmode);
	TEST_EQ(fmode, FILTER_MODE_IN);

	memset(&tr, 0, sizeof(tr));
	TEST_NE(uftrace_match_filter(0x4100, &root, &tr), NULL);
	TEST_EQ(tr.flags, TRIGGER_FL_FILTER);
	TEST_EQ(tr.fmode, FILTER_MODE_OUT);

	uftrace_cleanup_filter(&root);
	TEST_EQ(RB_EMPTY_ROOT(&root), true);

	return TEST_OK;
}

TEST_CASE(trigger_setup_args)
{
	struct symtabs stabs = {
		.loaded = false,
	};;
	struct rb_root root = RB_ROOT;
	struct rb_node *node;
	struct uftrace_filter *filter;
	struct uftrace_trigger tr;
	struct uftrace_arg_spec *spec;
	int count;

	filter_test_load_symtabs(&stabs);

	uftrace_setup_argument("foo::bar@arg1", &stabs, &root);
	TEST_EQ(RB_EMPTY_ROOT(&root), false);

	memset(&tr, 0, sizeof(tr));
	TEST_NE(uftrace_match_filter(0x2500, &root, &tr), NULL);
	TEST_EQ(tr.flags, TRIGGER_FL_ARGUMENT);
	TEST_NE(tr.pargs, NULL);

	uftrace_setup_trigger("foo::bar@arg2/s", &stabs, &root, NULL);
	memset(&tr, 0, sizeof(tr));
	TEST_NE(uftrace_match_filter(0x2500, &root, &tr), NULL);
	TEST_EQ(tr.flags, TRIGGER_FL_ARGUMENT);
	TEST_NE(tr.pargs, NULL);

	count = 0;
	list_for_each_entry(spec, tr.pargs, list) {
		count++;
		if (count == 1) {
			TEST_EQ(spec->idx, 1);
			TEST_EQ(spec->fmt, ARG_FMT_AUTO);
			TEST_EQ(spec->type, ARG_TYPE_INDEX);
		}
		else if (count == 2) {
			TEST_EQ(spec->idx, 2);
			TEST_EQ(spec->fmt, ARG_FMT_STR);
			TEST_EQ(spec->type, ARG_TYPE_INDEX);
		}
	}
	TEST_EQ(count, 2);

	uftrace_setup_argument("foo::baz1@arg1/i32,arg2/x64,fparg1/32,fparg2", &stabs, &root);
	memset(&tr, 0, sizeof(tr));
	TEST_NE(uftrace_match_filter(0x3999, &root, &tr), NULL);
	TEST_EQ(tr.flags, TRIGGER_FL_ARGUMENT);

	count = 0;
	list_for_each_entry(spec, tr.pargs, list) {
		switch (++count) {
		case 1:
			TEST_EQ(spec->idx, 1);
			TEST_EQ(spec->fmt, ARG_FMT_SINT);
			TEST_EQ(spec->type, ARG_TYPE_INDEX);
			TEST_EQ(spec->size, 4);
			break;
		case 2:
			TEST_EQ(spec->idx, 2);
			TEST_EQ(spec->fmt, ARG_FMT_HEX);
			TEST_EQ(spec->type, ARG_TYPE_INDEX);
			TEST_EQ(spec->size, 8);
			break;
		case 3:
			TEST_EQ(spec->idx, 1);
			TEST_EQ(spec->fmt, ARG_FMT_FLOAT);
			TEST_EQ(spec->type, ARG_TYPE_FLOAT);
			TEST_EQ(spec->size, 4);
			break;
		case 4:
			TEST_EQ(spec->idx, 2);
			TEST_EQ(spec->fmt, ARG_FMT_FLOAT);
			TEST_EQ(spec->type, ARG_TYPE_FLOAT);
			TEST_EQ(spec->size, 8);
			break;
		default:
			/* should not reach here */
			TEST_EQ(spec->idx, -1);
			break;
		}
	}
	TEST_EQ(count, 4);

	/* FIXME: this test will fail on non-x86 architecture */
	uftrace_setup_trigger("foo::baz2@arg1/c,arg2/x32%rdi,arg3%stack+4,retval/f64", &stabs, &root, NULL);
	memset(&tr, 0, sizeof(tr));
	TEST_NE(uftrace_match_filter(0x4000, &root, &tr), NULL);
	TEST_EQ(tr.flags, TRIGGER_FL_ARGUMENT | TRIGGER_FL_RETVAL);

	count = 0;
	list_for_each_entry(spec, tr.pargs, list) {
		switch (++count) {
		case 1:
			TEST_EQ(spec->idx, 1);
			TEST_EQ(spec->fmt, ARG_FMT_CHAR);
			TEST_EQ(spec->type, ARG_TYPE_INDEX);
			TEST_EQ(spec->size, 1);
			break;
		case 2:
			TEST_EQ(spec->idx, 2);
			TEST_EQ(spec->fmt, ARG_FMT_HEX);
			TEST_EQ(spec->type, ARG_TYPE_REG);
			TEST_EQ(spec->size, 4);
			/* XXX: x86-specific */
			TEST_EQ(spec->reg_idx, arch_register_index("rdi"));
			break;
		case 3:
			TEST_EQ(spec->idx, 3);
			TEST_EQ(spec->fmt, ARG_FMT_AUTO);
			TEST_EQ(spec->type, ARG_TYPE_STACK);
			TEST_EQ(spec->size, (int)sizeof(long));
			TEST_EQ(spec->stack_ofs, 4);
			break;
		case 4:
			TEST_EQ(spec->idx, 0);
			TEST_EQ(spec->fmt, ARG_FMT_FLOAT);
			TEST_EQ(spec->type, ARG_TYPE_FLOAT);
			TEST_EQ(spec->size, 8);
			break;
		default:
			/* should not reach here */
			TEST_EQ(spec->idx, -1);
			break;
		}
	}
	TEST_EQ(count, 4);

	uftrace_cleanup_filter(&root);
	TEST_EQ(RB_EMPTY_ROOT(&root), true);

	return TEST_OK;
}

#endif /* UNIT_TEST */
