/* Copyright (c) 2005-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "str.h"
#include "hash.h"
#include "ostream.h"
#include "settings-parser.h"
#include "master-service-settings.h"
#include "all-settings.h"
#include "config-parser.h"
#include "config-request.h"
#include "config-filter.h"
#include "old-set-parser.h"

struct config_export_context {
	pool_t pool;
	string_t *value;
	string_t *prefix;
	HASH_TABLE(char *, char *) keys;
	enum config_dump_scope scope;

	config_request_callback_t *callback;
	void *context;

	enum config_dump_flags flags;
	const struct config_module_parser *module_parsers;
	struct config_module_parser *dup_module_parsers;
	unsigned int section_idx;
};

static void config_export_size(string_t *str, uoff_t size)
{
	static const char suffixes[] = { 'B', 'k', 'M', 'G', 'T' };
	char suffix = suffixes[0];
	unsigned int i;

	if (size == 0) {
		str_append_c(str, '0');
		return;
	}
	for (i = 1; i < N_ELEMENTS(suffixes) && (size % 1024) == 0; i++) {
		suffix = suffixes[i];
		size /= 1024;
	}
	str_printfa(str, "%"PRIuUOFF_T" %c", size, suffix);
}

static void config_export_time(string_t *str, unsigned int stamp)
{
	const char *suffix = "secs";

	if (stamp == 0) {
		str_append_c(str, '0');
		return;
	}

	if (stamp % 60 == 0) {
		stamp /= 60;
		suffix = "mins";
		if (stamp % 60 == 0) {
			stamp /= 60;
			suffix = "hours";
			if (stamp % 24 == 0) {
				stamp /= 24;
				suffix = "days";
				if (stamp % 7 == 0) {
					stamp /= 7;
					suffix = "weeks";
				}
			}
		}
	}

	str_printfa(str, "%u %s", stamp, suffix);
}

static void config_export_time_msecs(string_t *str, unsigned int stamp_msecs)
{
	if ((stamp_msecs % 1000) == 0)
		config_export_time(str, stamp_msecs/1000);
	else
		str_printfa(str, "%u ms", stamp_msecs);
}

bool config_export_type(string_t *str, const void *value,
			const void *default_value,
			enum setting_type type, bool dump_default,
			bool *dump_r)
{
	switch (type) {
	case SET_BOOL: {
		const bool *val = value, *dval = default_value;

		if (dump_default || dval == NULL || *val != *dval)
			str_append(str, *val ? "yes" : "no");
		break;
	}
	case SET_SIZE: {
		const uoff_t *val = value, *dval = default_value;

		if (dump_default || dval == NULL || *val != *dval)
			config_export_size(str, *val);
		break;
	}
	case SET_UINT:
	case SET_UINT_OCT:
	case SET_TIME:
	case SET_TIME_MSECS: {
		const unsigned int *val = value, *dval = default_value;

		if (dump_default || dval == NULL || *val != *dval) {
			switch (type) {
			case SET_UINT_OCT:
				str_printfa(str, "0%o", *val);
				break;
			case SET_TIME:
				config_export_time(str, *val);
				break;
			case SET_TIME_MSECS:
				config_export_time_msecs(str, *val);
				break;
			default:
				str_printfa(str, "%u", *val);
				break;
			}
		}
		break;
	}
	case SET_IN_PORT: {
		const in_port_t *val = value, *dval = default_value;

		if (dump_default || dval == NULL || *val != *dval)
			str_printfa(str, "%u", *val);
		break;
	}
	case SET_STR_VARS: {
		const char *const *val = value, *sval;
		const char *const *_dval = default_value;
		const char *dval = _dval == NULL ? NULL : *_dval;

		i_assert(*val == NULL ||
			 **val == SETTING_STRVAR_UNEXPANDED[0]);

		sval = *val == NULL ? NULL : (*val + 1);
		if ((dump_default || null_strcmp(sval, dval) != 0) &&
		    sval != NULL) {
			str_append(str, sval);
			*dump_r = TRUE;
		}
		break;
	}
	case SET_STR: {
		const char *const *val = value;
		const char *const *_dval = default_value;
		const char *dval = _dval == NULL ? NULL : *_dval;

		if ((dump_default || null_strcmp(*val, dval) != 0) &&
		    *val != NULL) {
			str_append(str, *val);
			*dump_r = TRUE;
		}
		break;
	}
	case SET_ENUM: {
		const char *const *val = value;
		size_t len = strlen(*val);

		if (dump_default)
			str_append(str, *val);
		else {
			const char *const *_dval = default_value;
			const char *dval = _dval == NULL ? NULL : *_dval;

			i_assert(dval != NULL);
			if (strncmp(*val, dval, len) != 0 ||
			    ((*val)[len] != ':' && (*val)[len] != '\0'))
				str_append(str, *val);
		}
		break;
	}
	default:
		return FALSE;
	}
	return TRUE;
}

static void
setting_export_section_name(string_t *str, const struct setting_define *def,
			    const void *set, unsigned int idx)
{
	const char *const *name;
	size_t name_offset1;

	if (def->type != SET_DEFLIST_UNIQUE) {
		/* not unique, use the index */
		str_printfa(str, "%u", idx);
		return;
	}
	name_offset1 = def->list_info->type_offset1;
	i_assert(name_offset1 != 0);

	name = CONST_PTR_OFFSET(set, name_offset1 - 1);
	if (*name == NULL || **name == '\0') {
		/* no name, this one isn't unique. use the index. */
		str_printfa(str, "%u", idx);
	} else T_BEGIN {
		str_append(str, settings_section_escape(*name));
	} T_END;
}

static void
settings_export(struct config_export_context *ctx,
		const struct setting_parser_info *info,
		bool parent_unique_deflist,
		const void *set, const void *change_set)
{
	const struct setting_define *def;
	const void *value, *default_value, *change_value;
	void *const *children, *const *change_children = NULL;
	unsigned int i, count, count2;
	size_t prefix_len;
	const char *str;
	char *key;
	bool dump, dump_default = FALSE;

	for (def = info->defines; def->key != NULL; def++) {
		value = CONST_PTR_OFFSET(set, def->offset);
		default_value = info->defaults == NULL ? NULL :
			CONST_PTR_OFFSET(info->defaults, def->offset);
		change_value = CONST_PTR_OFFSET(change_set, def->offset);
		switch (ctx->scope) {
		case CONFIG_DUMP_SCOPE_ALL_WITH_HIDDEN:
			dump_default = TRUE;
			break;
		case CONFIG_DUMP_SCOPE_ALL_WITHOUT_HIDDEN:
			if ((def->flags & SET_FLAG_HIDDEN) == 0) {
				/* not hidden - dump it */
				dump_default = TRUE;
				break;
			}
			/* hidden - dump default only if it's explicitly set */
			/* fall through */
		case CONFIG_DUMP_SCOPE_SET:
			dump_default = *((const char *)change_value) != 0;
			break;
		case CONFIG_DUMP_SCOPE_CHANGED:
			dump_default = FALSE;
			break;
		}
		if (!parent_unique_deflist ||
		    (ctx->flags & CONFIG_DUMP_FLAG_HIDE_LIST_DEFAULTS) == 0) {
			/* .. */
		} else if (*((const char *)change_value) == 0 &&
			   def->offset + 1 != info->type_offset1) {
			/* this is mainly for service {} blocks. if value
			   hasn't changed, it's the default. even if
			   info->defaults has a different value. */
			default_value = value;
		} else {
			/* value is set explicitly, but we don't know the
			   default here. assume it's not the default. */
			dump_default = TRUE;
		}

		dump = FALSE;
		count = 0; children = NULL;
		str_truncate(ctx->value, 0);
		switch (def->type) {
		case SET_BOOL:
		case SET_SIZE:
		case SET_UINT:
		case SET_UINT_OCT:
		case SET_TIME:
		case SET_TIME_MSECS:
		case SET_IN_PORT:
		case SET_STR_VARS:
		case SET_STR:
		case SET_ENUM:
			if (!config_export_type(ctx->value, value,
						default_value, def->type,
						dump_default, &dump))
				i_unreached();
			break;
		case SET_DEFLIST:
		case SET_DEFLIST_UNIQUE: {
			const ARRAY_TYPE(void_array) *val = value;
			const ARRAY_TYPE(void_array) *change_val = change_value;

			if (!array_is_created(val))
				break;

			children = array_get(val, &count);
			for (i = 0; i < count; i++) {
				if (i > 0)
					str_append_c(ctx->value, ' ');
				setting_export_section_name(ctx->value, def, children[i],
							    ctx->section_idx + i);
			}
			change_children = array_get(change_val, &count2);
			i_assert(count == count2);
			break;
		}
		case SET_STRLIST: {
			const ARRAY_TYPE(const_string) *val = value;
			const char *const *strings;

			if (!array_is_created(val))
				break;

			key = p_strconcat(ctx->pool, str_c(ctx->prefix),
					  def->key, NULL);

			if (hash_table_lookup(ctx->keys, key) != NULL) {
				/* already added all of these */
				break;
			}
			if ((ctx->flags & CONFIG_DUMP_FLAG_DEDUPLICATE_KEYS) != 0)
				hash_table_insert(ctx->keys, key, key);
			/* for doveconf -n to see this KEY_LIST */
			ctx->callback(key, "", CONFIG_KEY_LIST, ctx->context);

			strings = array_get(val, &count);
			i_assert(count % 2 == 0);
			for (i = 0; i < count; i += 2) {
				str = p_strdup_printf(ctx->pool, "%s%s%c%s",
						      str_c(ctx->prefix),
						      def->key,
						      SETTINGS_SEPARATOR,
						      strings[i]);
				ctx->callback(str, strings[i+1],
					      CONFIG_KEY_NORMAL, ctx->context);
			}
			count = 0;
			break;
		}
		case SET_ALIAS:
			break;
		}
		if (str_len(ctx->value) > 0 || dump) {
			key = p_strconcat(ctx->pool, str_c(ctx->prefix),
					  def->key, NULL);
			if (hash_table_lookup(ctx->keys, key) == NULL) {
				enum config_key_type type;

				if (def->offset + 1 == info->type_offset1 &&
				    parent_unique_deflist)
					type = CONFIG_KEY_UNIQUE_KEY;
				else if (SETTING_TYPE_IS_DEFLIST(def->type))
					type = CONFIG_KEY_LIST;
				else
					type = CONFIG_KEY_NORMAL;
				ctx->callback(key, str_c(ctx->value), type,
					ctx->context);
				if ((ctx->flags & CONFIG_DUMP_FLAG_DEDUPLICATE_KEYS) != 0)
					hash_table_insert(ctx->keys, key, key);
			}
		}

		i_assert(count == 0 || children != NULL);
		prefix_len = str_len(ctx->prefix);
		unsigned int section_start_idx = ctx->section_idx;
		ctx->section_idx += count;
		for (i = 0; i < count; i++) {
			str_append(ctx->prefix, def->key);
			str_append_c(ctx->prefix, SETTINGS_SEPARATOR);
			setting_export_section_name(ctx->prefix, def, children[i],
						    section_start_idx + i);
			str_append_c(ctx->prefix, SETTINGS_SEPARATOR);
			settings_export(ctx, def->list_info,
					def->type == SET_DEFLIST_UNIQUE,
					children[i], change_children[i]);

			str_truncate(ctx->prefix, prefix_len);
		}
	}
}

struct config_export_context *
config_export_init(enum config_dump_scope scope,
		   enum config_dump_flags flags,
		   config_request_callback_t *callback, void *context)
{
	struct config_export_context *ctx;
	pool_t pool;

	pool = pool_alloconly_create(MEMPOOL_GROWING"config export", 1024*64);
	ctx = p_new(pool, struct config_export_context, 1);
	ctx->pool = pool;

	ctx->flags = flags;
	ctx->callback = callback;
	ctx->context = context;
	ctx->scope = scope;
	ctx->value = str_new(pool, 256);
	ctx->prefix = str_new(pool, 64);
	hash_table_create(&ctx->keys, ctx->pool, 0, str_hash, strcmp);
	return ctx;
}

static struct config_module_parser *
config_filter_parsers_dup(pool_t pool, struct config_filter_parser *global_filter)
{
	struct config_module_parser *dest;
	unsigned int i, count;

	for (count = 0; global_filter->module_parsers[count].root != NULL; count++) ;
	dest = p_new(pool, struct config_module_parser, count + 1);
	for (i = 0; i < count; i++) {
		dest[i] = global_filter->module_parsers[i];
		dest[i].parser =
			settings_parser_dup(global_filter->module_parsers[i].parser, pool);
	}
	return dest;
}

void config_export_dup_module_parsers(struct config_export_context *ctx,
				      struct config_parsed *config)
{
	struct config_filter_parser *global_filter =
		config_parsed_get_global_filter_parser(config);

	ctx->dup_module_parsers =
		config_filter_parsers_dup(ctx->pool, global_filter);
	ctx->module_parsers = ctx->dup_module_parsers;
}

void config_export_set_module_parsers(struct config_export_context *ctx,
				      const struct config_module_parser *module_parsers)
{
	ctx->module_parsers = module_parsers;
}

unsigned int config_export_get_parser_count(struct config_export_context *ctx)
{
	unsigned int i = 0;
	for (i = 0; ctx->module_parsers[i].root != NULL; i++) ;
	return i;
}

const char *
config_export_get_import_environment(struct config_export_context *ctx)
{
	enum setting_type stype;
	unsigned int i;

	for (i = 0; ctx->module_parsers[i].root != NULL; i++) {
		if (ctx->module_parsers[i].root == &master_service_setting_parser_info) {
			const char *const *value =
				settings_parse_get_value(ctx->module_parsers[i].parser,
					"import_environment", &stype);
			i_assert(value != NULL);
			return *value;
		}
	}
	i_unreached();
}

const char *config_export_get_base_dir(struct config_export_context *ctx)
{
	enum setting_type stype;
	unsigned int i;

	for (i = 0; ctx->module_parsers[i].root != NULL; i++) {
		if (ctx->module_parsers[i].root == &master_service_setting_parser_info) {
			const char *const *value =
				settings_parse_get_value(ctx->module_parsers[i].parser,
					"base_dir", &stype);
			i_assert(value != NULL);
			return *value;
		}
	}
	i_unreached();
}

void config_export_free(struct config_export_context **_ctx)
{
	struct config_export_context *ctx = *_ctx;

	*_ctx = NULL;

	if (ctx->dup_module_parsers != NULL)
		config_module_parsers_free(ctx->dup_module_parsers);
	hash_table_destroy(&ctx->keys);
	pool_unref(&ctx->pool);
}

int config_export_all_parsers(struct config_export_context **_ctx,
			      unsigned int *section_idx)
{
	struct config_export_context *ctx = *_ctx;
	const char *error;
	unsigned int i;
	int ret = 0;

	*_ctx = NULL;

	for (i = 0; ctx->module_parsers[i].root != NULL; i++) {
		if (config_export_parser(ctx, i, section_idx, &error) < 0) {
			i_error("%s", error);
			ret = -1;
			break;
		}
	}
	config_export_free(&ctx);
	return ret;
}

const struct setting_parser_info *
config_export_parser_get_info(struct config_export_context *ctx,
			      unsigned int parser_idx)
{
	return ctx->module_parsers[parser_idx].root;
}

int config_export_parser(struct config_export_context *ctx,
			 unsigned int parser_idx,
			 unsigned int *section_idx, const char **error_r)
{
	const struct config_module_parser *module_parser =
		&ctx->module_parsers[parser_idx];

	if (module_parser->delayed_error != NULL) {
		*error_r = module_parser->delayed_error;
		return -1;
	}

	ctx->section_idx = *section_idx;
	T_BEGIN {
		void *set = settings_parser_get_set(module_parser->parser);
		settings_export(ctx, module_parser->root, FALSE, set,
				settings_parser_get_changes(module_parser->parser));
	} T_END;

	*section_idx = ctx->section_idx;
	return 0;
}
