#include "stdafx.h"
#include "Emu/emu_callbacks.h"
#include "Emu/System.h"
#include "Emu/system_config.h"
#include "Emu/VFS.h"
#include "Emu/IdManager.h"
#include "Emu/Cell/PPUModule.h"
#include "Emu/Cell/timers.hpp"
#include "Emu/Cell/Modules/cellMsgDialog.h"

#include "Utilities/rXml.h"
#include "Loader/TRP.h"
#include "Loader/TROPUSR.h"

#include "sceNp.h"
#include "sceNpTrophy.h"
#include "cellSysutil.h"
#include "Emu/NP/np_handler.h"

#include "Utilities/StrUtil.h"
#include "util/yaml.hpp"

#include "Emu/Cell/lv2/sys_event.h"
#include "Emu/Cell/lv2/sys_fs.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <functional>
#include <shared_mutex>
#include "util/asm.hpp"
#include "util/cctype.hpp"

LOG_CHANNEL(sceNpTrophy);

TrophyNotificationBase::~TrophyNotificationBase()
{
}

struct trophy_context_t
{
	static const u32 id_base = 1;
	static const u32 id_step = 1;
	static const u32 id_count = 4;
	SAVESTATE_INIT_POS(42);

	std::string trp_name;
	SceNpCommunicationId comm_id{};   // set at CreateContext, not serialized
	std::unique_ptr<TROPUSRLoader> tropusr;
	bool read_only = false;

	trophy_context_t() = default;

	trophy_context_t(utils::serial& ar)
		: trp_name(ar.pop<std::string>())
	{
		std::string trophy_path = vfs::get(Emu.GetDir() + "TROPDIR/" + trp_name + "/TROPHY.TRP");
		fs::file trp_stream(trophy_path);

		if (!trp_stream)
		{
			// Fallback
			trophy_path = vfs::get("/dev_bdvd/PS3_GAME/TROPDIR/" + trp_name + "/TROPHY.TRP");
			trp_stream.open(trophy_path);
		}

		if (!ar.pop<bool>())
		{
			ar(read_only);
			return;
		}

		ar(read_only);

		if (!trp_stream && g_cfg.savestate.state_inspection_mode)
		{
			return;
		}

		const std::string trophyPath = "/dev_hdd0/home/" + Emu.GetUsr() + "/trophy/" + trp_name;
		tropusr = std::make_unique<TROPUSRLoader>();
		const std::string trophyUsrPath = trophyPath + "/TROPUSR.DAT";
		const std::string trophyConfPath = trophyPath + "/TROPCONF.SFM";
		ensure(tropusr->Load(trophyUsrPath, trophyConfPath).success);
	}

	void save(utils::serial& ar)
	{
		ar(trp_name, tropusr.operator bool(), read_only);
	}
};

struct trophy_handle_t
{
	static const u32 id_base = 1;
	static const u32 id_step = 1;
	static const u32 id_count = 4;
	SAVESTATE_INIT_POS(43);

	bool is_aborted = false;

	trophy_handle_t() = default;

	trophy_handle_t(utils::serial& ar)
		: is_aborted(ar)
	{
	}

	void save(utils::serial& ar)
	{
		ar(is_aborted);
	}
};

struct sce_np_trophy_manager
{
	shared_mutex mtx;
	atomic_t<bool> is_initialized = false;

	// Get context + check handle given
	static std::pair<trophy_context_t*, SceNpTrophyError> get_context_ex(u32 context, u32 handle, bool test_writeable = false)
	{
		decltype(get_context_ex(0, 0)) res{};
		auto& [ctxt, error] = res;

		if (context < trophy_context_t::id_base ||
			context >= trophy_context_t::id_base + trophy_context_t::id_count)
		{
			// Id was not in range of valid ids
			error = SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
			return res;
		}

		ctxt = idm::check_unlocked<trophy_context_t>(context);

		if (!ctxt)
		{
			error = SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT;
			return res;
		}

		if (test_writeable && ctxt->read_only)
		{
			error = SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;
			return res;
		}

		if (handle < trophy_handle_t::id_base ||
			handle >= trophy_handle_t::id_base + trophy_handle_t::id_count)
		{
			error = SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
			return res;
		}

		const auto hndl = idm::check_unlocked<trophy_handle_t>(handle);

		if (!hndl)
		{
			error = SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE;
			return res;
		}
		else if (hndl->is_aborted)
		{
			error = SCE_NP_TROPHY_ERROR_ABORT;
			return res;
		}

		return res;
	}

	SAVESTATE_INIT_POS(12);

	sce_np_trophy_manager() = default;

	sce_np_trophy_manager(utils::serial& ar)
		: is_initialized(ar.pop<bool>())
	{
	}

	void save(utils::serial& ar)
	{
		ar(is_initialized);

		if (is_initialized)
		{
			USING_SERIALIZATION_VERSION(sceNpTrophy);
		}
	}
};

template<>
void fmt_class_string<SceNpTrophyError>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto error)
	{
		switch (error)
		{
		STR_CASE(SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED);
		STR_CASE(SCE_NP_TROPHY_ERROR_NOT_INITIALIZED);
		STR_CASE(SCE_NP_TROPHY_ERROR_NOT_SUPPORTED);
		STR_CASE(SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED);
		STR_CASE(SCE_NP_TROPHY_ERROR_OUT_OF_MEMORY);
		STR_CASE(SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
		STR_CASE(SCE_NP_TROPHY_ERROR_EXCEEDS_MAX);
		STR_CASE(SCE_NP_TROPHY_ERROR_INSUFFICIENT);
		STR_CASE(SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT);
		STR_CASE(SCE_NP_TROPHY_ERROR_INVALID_FORMAT);
		STR_CASE(SCE_NP_TROPHY_ERROR_BAD_RESPONSE);
		STR_CASE(SCE_NP_TROPHY_ERROR_INVALID_GRADE);
		STR_CASE(SCE_NP_TROPHY_ERROR_INVALID_CONTEXT);
		STR_CASE(SCE_NP_TROPHY_ERROR_PROCESSING_ABORTED);
		STR_CASE(SCE_NP_TROPHY_ERROR_ABORT);
		STR_CASE(SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE);
		STR_CASE(SCE_NP_TROPHY_ERROR_LOCKED);
		STR_CASE(SCE_NP_TROPHY_ERROR_HIDDEN);
		STR_CASE(SCE_NP_TROPHY_ERROR_CANNOT_UNLOCK_PLATINUM);
		STR_CASE(SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED);
		STR_CASE(SCE_NP_TROPHY_ERROR_INVALID_TYPE);
		STR_CASE(SCE_NP_TROPHY_ERROR_INVALID_HANDLE);
		STR_CASE(SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID);
		STR_CASE(SCE_NP_TROPHY_ERROR_UNKNOWN_NP_COMM_ID);
		STR_CASE(SCE_NP_TROPHY_ERROR_DISC_IO);
		STR_CASE(SCE_NP_TROPHY_ERROR_CONF_DOES_NOT_EXIST);
		STR_CASE(SCE_NP_TROPHY_ERROR_UNSUPPORTED_FORMAT);
		STR_CASE(SCE_NP_TROPHY_ERROR_ALREADY_INSTALLED);
		STR_CASE(SCE_NP_TROPHY_ERROR_BROKEN_DATA);
		STR_CASE(SCE_NP_TROPHY_ERROR_VERIFICATION_FAILURE);
		STR_CASE(SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID);
		STR_CASE(SCE_NP_TROPHY_ERROR_UNKNOWN_TROPHY_ID);
		STR_CASE(SCE_NP_TROPHY_ERROR_UNKNOWN_TITLE);
		STR_CASE(SCE_NP_TROPHY_ERROR_UNKNOWN_FILE);
		STR_CASE(SCE_NP_TROPHY_ERROR_DISC_NOT_MOUNTED);
		STR_CASE(SCE_NP_TROPHY_ERROR_SHUTDOWN);
		STR_CASE(SCE_NP_TROPHY_ERROR_TITLE_ICON_NOT_FOUND);
		STR_CASE(SCE_NP_TROPHY_ERROR_TROPHY_ICON_NOT_FOUND);
		STR_CASE(SCE_NP_TROPHY_ERROR_INSUFFICIENT_DISK_SPACE);
		STR_CASE(SCE_NP_TROPHY_ERROR_ILLEGAL_UPDATE);
		STR_CASE(SCE_NP_TROPHY_ERROR_SAVEDATA_USER_DOES_NOT_MATCH);
		STR_CASE(SCE_NP_TROPHY_ERROR_TROPHY_ID_DOES_NOT_EXIST);
		STR_CASE(SCE_NP_TROPHY_ERROR_SERVICE_UNAVAILABLE);
		STR_CASE(SCE_NP_TROPHY_ERROR_UNKNOWN);
		}

		return unknown;
	});
}

template <>
void fmt_class_string<SceNpCommunicationSignature>::format(std::string& out, u64 arg)
{
	const auto& sign = get_object(arg);

	fmt::append(out, "%s", sign.data);
}

template <>
void fmt_class_string<SceNpCommunicationId>::format(std::string& out, u64 arg)
{
	const auto& id = get_object(arg);

	const u8 term = id.data[9];
	fmt::append(out, "{ data='%s', term='%s' (0x%x), num=%d, dummy=%d }", id.data, utils::isprint(term) ? fmt::format("%c", term) : "", term, id.num, id.dummy);
}

// Helpers

static error_code NpTrophyGetTrophyInfo(const trophy_context_t* ctxt, s32 trophyId, SceNpTrophyDetails* details, SceNpTrophyData* data);

static void show_trophy_notification(const trophy_context_t* ctxt, s32 trophyId)
{
	// Get icon for the notification.
	const std::string padded_trophy_id = fmt::format("%03u", trophyId);
	const std::string trophy_icon_path = "/dev_hdd0/home/" + Emu.GetUsr() + "/trophy/" + ctxt->trp_name + "/TROP" + padded_trophy_id + ".PNG";
	fs::file trophy_icon_file = fs::file(vfs::get(trophy_icon_path));
	std::vector<uchar> trophy_icon_data;
	trophy_icon_file.read(trophy_icon_data, trophy_icon_file.size());

	SceNpTrophyDetails details{};

	if (const auto ret = NpTrophyGetTrophyInfo(ctxt, trophyId, &details, nullptr))
	{
		sceNpTrophy.error("Failed to get info for trophy dialog. Error code 0x%x", +ret);
	}

	if (auto trophy_notification_dialog = g_emu_callbacks.get_trophy_notification_dialog())
	{
		trophy_notification_dialog->ShowTrophyNotification(details, trophy_icon_data);
	}
}

// ---------------------------------------------------------------------------------------
// Custom trophy rules (ARMSX3-Trophies)
//
// An optional rules file lets a trophy pack award trophies from the emulator side, by
// watching guest memory, instead of patching game code. It can also "remove" trophies
// (e.g. ones that need long-dead online servers): the game still sees them, but they are
// never awarded, are hidden from the trophy list and no longer block the platinum.
//
// The file is looked up in this order:
//   /dev_hdd0/home/<user>/trophy_rules/<NPWRxxxxx_00>.yml   (handy while writing rules)
//   /dev_hdd0/home/<user>/trophy/<NPWRxxxxx_00>/TROPRULES.YML (shipped inside TROPHY.TRP)
//
//   game: PPU-becb727c...        # optional: memory rules only run on this executable
//   original_count: 14           # optional: ids from here on are custom trophies
//   remove: [5, 9]               # optional
//   trophies:
//     14:
//       when: "[[0xF3F20]+0x48] >= 50"
//
// Without a rules file nothing here changes any behaviour.
// ---------------------------------------------------------------------------------------

enum ct_op_type : u8
{
	ct_push,
	ct_add,
	ct_sub,
	ct_mul,
	ct_and,
	ct_deref,
};

struct ct_op
{
	u8 type = ct_push;
	f64 value = 0;
	u8 size = 4;
	bool is_signed = false;
	bool is_float = false;
};

enum ct_cmp_type : u8
{
	ct_eq,
	ct_ne,
	ct_ge,
	ct_le,
	ct_gt,
	ct_lt,
};

struct ct_condition
{
	std::vector<ct_op> lhs;
	std::vector<ct_op> rhs;
	u8 cmp = ct_eq;
};

struct ct_rule
{
	s32 id = -1;
	std::vector<ct_condition> all;
	std::string text;
};

struct ct_rules_set
{
	bool loaded = false;
	bool platinum_checked = false;
	s32 original_count = -1;
	std::set<u32> removed;
	std::vector<ct_rule> rules;

	bool active() const
	{
		return !removed.empty() || !rules.empty();
	}

	bool has_rule(s32 id) const
	{
		for (const ct_rule& rule : rules)
		{
			if (rule.id == id)
			{
				return true;
			}
		}

		return false;
	}

	// Trophies the real servers know nothing about (or that were not earned the official way)
	bool is_custom(s32 id) const
	{
		return (original_count >= 0 && id >= original_count) || has_rule(id);
	}

	bool is_removed(s32 id) const
	{
		return id >= 0 && removed.count(static_cast<u32>(id)) != 0;
	}
};

// Only accessed while sce_np_trophy_manager::mtx is held
struct custom_trophy_state
{
	std::map<std::string, ct_rules_set> sets;

	custom_trophy_state() = default;
	custom_trophy_state(const custom_trophy_state&) = delete;
	custom_trophy_state& operator=(const custom_trophy_state&) = delete;
};

struct ct_parser
{
	std::string_view s;
	usz pos = 0;
	std::vector<ct_op>* out = nullptr;

	void skip()
	{
		while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t'))
		{
			pos++;
		}
	}

	bool eat(char c)
	{
		skip();

		if (pos < s.size() && s[pos] == c)
		{
			pos++;
			return true;
		}

		return false;
	}

	bool at_end()
	{
		skip();
		return pos >= s.size();
	}

	bool expr()
	{
		if (!term())
		{
			return false;
		}

		while (true)
		{
			u8 type = ct_push;

			if (eat('+'))
			{
				type = ct_add;
			}
			else if (eat('-'))
			{
				type = ct_sub;
			}
			else
			{
				return true;
			}

			if (!term())
			{
				return false;
			}

			ct_op op{};
			op.type = type;
			out->push_back(op);
		}
	}

	bool term()
	{
		if (!factor())
		{
			return false;
		}

		while (true)
		{
			u8 type = ct_push;

			if (eat('*'))
			{
				type = ct_mul;
			}
			else if (eat('&'))
			{
				type = ct_and;
			}
			else
			{
				return true;
			}

			if (!factor())
			{
				return false;
			}

			ct_op op{};
			op.type = type;
			out->push_back(op);
		}
	}

	bool factor()
	{
		skip();

		if (pos >= s.size())
		{
			return false;
		}

		if (eat('('))
		{
			return expr() && eat(')');
		}

		if (eat('-'))
		{
			if (!factor())
			{
				return false;
			}

			ct_op neg{};
			neg.value = -1;
			out->push_back(neg);

			ct_op mul{};
			mul.type = ct_mul;
			out->push_back(mul);
			return true;
		}

		ct_op deref{};
		deref.type = ct_deref;

		struct type_name
		{
			std::string_view name;
			u8 size;
			bool is_signed;
			bool is_float;
		};

		static constexpr type_name s_types[] =
		{
			{"u8", 1, false, false},
			{"s8", 1, true, false},
			{"u16", 2, false, false},
			{"s16", 2, true, false},
			{"u32", 4, false, false},
			{"s32", 4, true, false},
			{"f32", 4, false, true},
		};

		for (const type_name& type : s_types)
		{
			if (s.substr(pos).starts_with(type.name))
			{
				usz next = pos + type.name.size();

				while (next < s.size() && (s[next] == ' ' || s[next] == '\t'))
				{
					next++;
				}

				if (next < s.size() && s[next] == '[')
				{
					pos = next;
					deref.size = type.size;
					deref.is_signed = type.is_signed;
					deref.is_float = type.is_float;
					break;
				}
			}
		}

		if (eat('['))
		{
			if (!expr() || !eat(']'))
			{
				return false;
			}

			out->push_back(deref);
			return true;
		}

		// Number: decimal, decimal fraction or 0x hex
		std::string token;

		while (pos < s.size() && ((s[pos] >= '0' && s[pos] <= '9') || (s[pos] >= 'a' && s[pos] <= 'f') || (s[pos] >= 'A' && s[pos] <= 'F') || s[pos] == 'x' || s[pos] == 'X' || s[pos] == '.'))
		{
			token += s[pos++];
		}

		if (token.empty())
		{
			return false;
		}

		ct_op num{};
		char* end = nullptr;

		if (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X'))
		{
			num.value = static_cast<f64>(std::strtoull(token.c_str() + 2, &end, 16));
		}
		else
		{
			num.value = std::strtod(token.c_str(), &end);
		}

		if (!end || *end != '\0')
		{
			return false;
		}

		out->push_back(num);
		return true;
	}
};

static bool ct_parse_condition(std::string_view text, ct_condition& cond)
{
	ct_parser parser{};
	parser.s = text;
	parser.out = &cond.lhs;

	if (!parser.expr())
	{
		return false;
	}

	parser.skip();

	const std::string_view rest = parser.s.substr(parser.pos);

	if (rest.starts_with("=="))
	{
		cond.cmp = ct_eq;
		parser.pos += 2;
	}
	else if (rest.starts_with("!="))
	{
		cond.cmp = ct_ne;
		parser.pos += 2;
	}
	else if (rest.starts_with(">="))
	{
		cond.cmp = ct_ge;
		parser.pos += 2;
	}
	else if (rest.starts_with("<="))
	{
		cond.cmp = ct_le;
		parser.pos += 2;
	}
	else if (rest.starts_with(">"))
	{
		cond.cmp = ct_gt;
		parser.pos += 1;
	}
	else if (rest.starts_with("<"))
	{
		cond.cmp = ct_lt;
		parser.pos += 1;
	}
	else if (rest.starts_with("="))
	{
		cond.cmp = ct_eq;
		parser.pos += 1;
	}
	else
	{
		return false;
	}

	parser.out = &cond.rhs;

	return parser.expr() && parser.at_end();
}

// Evaluate against guest memory. Fails (false) if anything points at unmapped memory.
static bool ct_eval(const std::vector<ct_op>& ops, f64& result)
{
	f64 stack[16]{};
	u32 sp = 0;

	for (const ct_op& op : ops)
	{
		switch (op.type)
		{
		case ct_push:
		{
			if (sp >= std::size(stack))
			{
				return false;
			}

			stack[sp++] = op.value;
			break;
		}
		case ct_add:
		case ct_sub:
		case ct_mul:
		case ct_and:
		{
			if (sp < 2)
			{
				return false;
			}

			const f64 b = stack[--sp];
			const f64 a = stack[sp - 1];

			if (op.type == ct_add)
			{
				stack[sp - 1] = a + b;
			}
			else if (op.type == ct_sub)
			{
				stack[sp - 1] = a - b;
			}
			else if (op.type == ct_mul)
			{
				stack[sp - 1] = a * b;
			}
			else
			{
				if (!(a >= -9.0e15 && a <= 9.0e15 && b >= -9.0e15 && b <= 9.0e15))
				{
					return false;
				}

				stack[sp - 1] = static_cast<f64>(static_cast<s64>(a) & static_cast<s64>(b));
			}

			break;
		}
		case ct_deref:
		{
			if (sp < 1)
			{
				return false;
			}

			const f64 a = stack[sp - 1];

			if (!(a >= 65536.0 && a <= 4294967295.0))
			{
				// Also treats null (and near-null) pointers as "not there yet"
				return false;
			}

			const u32 addr = static_cast<u32>(a);
			u8 buf[4]{};

			if (op.size < 1 || op.size > 4 || !vm::try_access(addr, buf, op.size, false))
			{
				return false;
			}

			if (op.size == 1)
			{
				stack[sp - 1] = op.is_signed ? static_cast<f64>(static_cast<s8>(buf[0])) : static_cast<f64>(buf[0]);
			}
			else if (op.size == 2)
			{
				const u16 v = static_cast<u16>((buf[0] << 8) | buf[1]);
				stack[sp - 1] = op.is_signed ? static_cast<f64>(static_cast<s16>(v)) : static_cast<f64>(v);
			}
			else
			{
				const u32 v = (u32{buf[0]} << 24) | (u32{buf[1]} << 16) | (u32{buf[2]} << 8) | u32{buf[3]};

				if (op.is_float)
				{
					f32 f = 0;
					std::memcpy(&f, &v, sizeof(f));
					stack[sp - 1] = f;
				}
				else
				{
					stack[sp - 1] = op.is_signed ? static_cast<f64>(static_cast<s32>(v)) : static_cast<f64>(v);
				}
			}

			break;
		}
		default:
		{
			return false;
		}
		}
	}

	if (sp != 1 || stack[0] != stack[0])
	{
		return false;
	}

	result = stack[0];
	return true;
}

static bool ct_test(const ct_rule& rule)
{
	if (rule.all.empty())
	{
		return false;
	}

	for (const ct_condition& cond : rule.all)
	{
		f64 a = 0;
		f64 b = 0;

		if (!ct_eval(cond.lhs, a) || !ct_eval(cond.rhs, b))
		{
			return false;
		}

		bool ok = false;

		switch (cond.cmp)
		{
		case ct_eq: ok = a == b; break;
		case ct_ne: ok = a != b; break;
		case ct_ge: ok = a >= b; break;
		case ct_le: ok = a <= b; break;
		case ct_gt: ok = a > b; break;
		case ct_lt: ok = a < b; break;
		default: break;
		}

		if (!ok)
		{
			return false;
		}
	}

	return true;
}

static bool ct_parse_int(const std::string& text, s32& out)
{
	if (text.empty())
	{
		return false;
	}

	char* end = nullptr;
	const bool hex = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
	const long value = hex ? std::strtol(text.c_str() + 2, &end, 16) : std::strtol(text.c_str(), &end, 10);

	if (!end || *end != '\0' || value < 0 || value > 0xffff)
	{
		return false;
	}

	out = static_cast<s32>(value);
	return true;
}

static void ct_add_conditions(ct_rule& rule, const std::string& text, bool& valid)
{
	for (const std::string& part : fmt::split(text, {"&&"}))
	{
		ct_condition cond{};

		if (!ct_parse_condition(part, cond))
		{
			sceNpTrophy.error("Custom trophy rules: trophy %d: cannot understand condition '%s'", rule.id, part);
			valid = false;
			return;
		}

		rule.all.emplace_back(std::move(cond));

		if (!rule.text.empty())
		{
			rule.text += " && ";
		}

		rule.text += part;
	}
}

static void ct_load(ct_rules_set& set, const std::string& trp_name)
{
	set = {};
	set.loaded = true;

	const std::string home = "/dev_hdd0/home/" + Emu.GetUsr();
	const std::string paths[] =
	{
		vfs::get(home + "/trophy_rules/" + trp_name + ".yml"),
		vfs::get(home + "/trophy/" + trp_name + "/TROPRULES.YML"),
	};

	std::string text;
	std::string used_path;

	for (const std::string& path : paths)
	{
		if (path.empty())
		{
			continue;
		}

		if (fs::file file{path})
		{
			text = file.to_string();
			used_path = path;
			break;
		}
	}

	if (used_path.empty())
	{
		return;
	}

	// Note: the core is built without C++ exceptions, so only non-throwing yaml-cpp calls are used here
	const auto loaded = yaml_load(text);
	const YAML::Node& root = loaded.first;

	if (!loaded.second.empty() || !root.IsMap())
	{
		sceNpTrophy.error("Custom trophy rules: failed to read '%s': %s", used_path, loaded.second);
		return;
	}

	if (const YAML::Node node = root["original_count"]; node && node.IsScalar())
	{
		s32 value = -1;

		if (ct_parse_int(node.Scalar(), value))
		{
			set.original_count = value;
		}
	}

	if (const YAML::Node node = root["remove"]; node && node.IsSequence())
	{
		for (const auto& entry : node)
		{
			s32 value = -1;

			if (entry.IsScalar() && ct_parse_int(entry.Scalar(), value))
			{
				set.removed.insert(static_cast<u32>(value));
			}
		}
	}

	bool rules_allowed = true;

	if (const YAML::Node node = root["game"]; node && node.IsScalar())
	{
		const std::string wanted = node.Scalar();
		const std::string& actual = Emu.GetExecutableHash();

		if (!wanted.empty() && wanted != actual)
		{
			sceNpTrophy.error("Custom trophy rules: '%s' is for %s but this game is %s. Memory rules are disabled.", used_path, wanted, actual);
			rules_allowed = false;
		}
	}

	if (const YAML::Node node = root["trophies"]; rules_allowed && node && node.IsMap())
	{
		for (const auto& entry : node)
		{
			ct_rule rule{};

			if (!entry.first.IsScalar() || !ct_parse_int(entry.first.Scalar(), rule.id))
			{
				sceNpTrophy.error("Custom trophy rules: bad trophy id in '%s'", used_path);
				continue;
			}

			const YAML::Node& value = entry.second;
			const YAML::Node when = value.IsMap() ? value["when"] : value;
			bool valid = true;

			if (when && when.IsScalar())
			{
				ct_add_conditions(rule, when.Scalar(), valid);
			}
			else if (when && when.IsSequence())
			{
				for (const auto& item : when)
				{
					if (valid && item.IsScalar())
					{
						ct_add_conditions(rule, item.Scalar(), valid);
					}
				}
			}

			if (!valid || rule.all.empty() || set.is_removed(rule.id))
			{
				continue;
			}

			sceNpTrophy.notice("Custom trophy rules: trophy %d when %s", rule.id, rule.text);
			set.rules.emplace_back(std::move(rule));
		}
	}

	sceNpTrophy.success("Custom trophy rules: loaded '%s' (%d rules, %d removed)", used_path, set.rules.size(), set.removed.size());
}

// Must be called with sce_np_trophy_manager::mtx held
static ct_rules_set& ct_get_rules(const std::string& trp_name, bool reload = false)
{
	ct_rules_set& set = g_fxo->get<custom_trophy_state>().sets[trp_name];

	if (reload || !set.loaded)
	{
		ct_load(set, trp_name);
	}

	return set;
}

// Shared by the game's own unlock requests and by the custom rules.
// Must be called with sce_np_trophy_manager::mtx held, on a PPU thread.
// With unlock_self=false only the platinum linked to trophyId is (re)checked.
// Returns the id of a platinum unlocked as a result, or SCE_NP_TROPHY_INVALID_TROPHY_ID.
static u32 np_trophy_unlock_impl(ppu_thread& ppu, trophy_context_t* ctxt, const ct_rules_set& rules, s32 trophyId, bool unlock_self)
{
	vm::var<CellRtcTick> tick;
	if (error_code error = cellRtcGetCurrentTick(ppu, tick))
	{
		sceNpTrophy.error("sceNpTrophyUnlockTrophy: Failed to get timestamp: 0x%x", +error);
	}

	if (unlock_self && ctxt->tropusr->UnlockTrophy(trophyId, tick->tick, tick->tick))
	{
		sceNpTrophy.notice("Trophy %d unlocked", trophyId);
	}

	// TODO: Make sure that unlocking platinum trophies is properly implemented and improve upon it
	const std::string& config_path = vfs::get("/dev_hdd0/home/" + Emu.GetUsr() + "/trophy/" + ctxt->trp_name + "/TROPCONF.SFM");
	const u32 unlocked_platinum_id = ctxt->tropusr->GetUnlockedPlatinumID(trophyId, config_path, rules.removed.empty() ? nullptr : &rules.removed);

	if (unlocked_platinum_id != SCE_NP_TROPHY_INVALID_TROPHY_ID)
	{
		sceNpTrophy.warning("sceNpTrophyUnlockTrophy: All requirements for unlocking the platinum trophy (ID = %d) were met.)", unlocked_platinum_id);

		if (ctxt->tropusr->UnlockTrophy(unlocked_platinum_id, tick->tick, tick->tick))
		{
			sceNpTrophy.success("You unlocked a platinum trophy! Hooray!!!");
		}
	}

	if (!unlock_self && unlocked_platinum_id == SCE_NP_TROPHY_INVALID_TROPHY_ID)
	{
		// Nothing changed
		return unlocked_platinum_id;
	}

	const std::string trophyPath = "/dev_hdd0/home/" + Emu.GetUsr() + "/trophy/" + ctxt->trp_name + "/TROPUSR.DAT";
	if (!ctxt->tropusr->Save(trophyPath))
	{
		sceNpTrophy.error("sceNpTrophyUnlockTrophy: failed to save '%s'", trophyPath);
	}

	if (g_cfg.misc.show_trophy_popups)
	{
		if (unlock_self)
		{
			// Enqueue popup for the regular trophy
			show_trophy_notification(ctxt, trophyId);
		}

		if (unlocked_platinum_id != SCE_NP_TROPHY_INVALID_TROPHY_ID)
		{
			// Enqueue popup for the holy platinum trophy
			show_trophy_notification(ctxt, unlocked_platinum_id);
		}
	}

	if (g_cfg.net.psn_status == np_psn_status::psn_rpcn)
	{
		// Custom trophies, and a platinum earned with a modified trophy set, stay on this device
		auto& np = g_fxo->get<named_thread<np::np_handler>>();

		if (unlock_self && !rules.is_custom(trophyId))
			np.rpcn_trophy_unlock(ctxt->comm_id, trophyId, static_cast<s64>(tick->tick));

		if (unlocked_platinum_id != SCE_NP_TROPHY_INVALID_TROPHY_ID && !rules.active())
			np.rpcn_trophy_unlock(ctxt->comm_id, static_cast<s32>(unlocked_platinum_id), static_cast<s64>(tick->tick));
	}

	return unlocked_platinum_id;
}

// Called regularly from cellSysutilCheckCallback (which games call about once per frame)
void sceNpTrophy_check_custom_rules(ppu_thread& ppu)
{
	static atomic_t<u64> s_next_check = 0;

	const u64 now = get_system_time();

	if (now < s_next_check)
	{
		return;
	}

	s_next_check = now + 250'000;

	auto& trophy_manager = g_fxo->get<sce_np_trophy_manager>();

	std::unique_lock lock(trophy_manager.mtx, std::try_to_lock);

	if (!lock || !trophy_manager.is_initialized)
	{
		return;
	}

	for (u32 context = trophy_context_t::id_base; context < trophy_context_t::id_base + trophy_context_t::id_count; context++)
	{
		const auto ctxt = idm::check_unlocked<trophy_context_t>(context);

		if (!ctxt || ctxt->read_only || !ctxt->tropusr)
		{
			continue;
		}

		ct_rules_set& rules = ct_get_rules(ctxt->trp_name);

		if (!rules.active())
		{
			continue;
		}

		const s32 count = static_cast<s32>(ctxt->tropusr->GetTrophiesCount());

		if (!rules.platinum_checked)
		{
			// Removing trophies may have completed a platinum that was already earned otherwise
			rules.platinum_checked = true;

			for (const u32 id : rules.removed)
			{
				if (id < static_cast<u32>(count))
				{
					np_trophy_unlock_impl(ppu, ctxt, rules, static_cast<s32>(id), false);
				}
			}
		}

		for (const ct_rule& rule : rules.rules)
		{
			if (rule.id < 0 || rule.id >= count)
			{
				continue;
			}

			if (ctxt->tropusr->GetTrophyUnlockState(rule.id) || ctxt->tropusr->GetTrophyGrade(rule.id) == SCE_NP_TROPHY_GRADE_PLATINUM)
			{
				continue;
			}

			if (!ct_test(rule))
			{
				continue;
			}

			sceNpTrophy.success("Custom trophy rules: trophy %d earned (%s)", rule.id, rule.text);
			np_trophy_unlock_impl(ppu, ctxt, rules, rule.id, true);
		}
	}
}

// Functions

error_code sceNpTrophyInit(vm::ptr<void> pool, u32 poolSize, u32 containerId, u64 options)
{
	sceNpTrophy.warning("sceNpTrophyInit(pool=*0x%x, poolSize=0x%x, containerId=0x%x, options=0x%llx)", pool, poolSize, containerId, options);

	auto& trophy_manager = g_fxo->get<sce_np_trophy_manager>();

	std::scoped_lock lock(trophy_manager.mtx);

	if (trophy_manager.is_initialized)
	{
		return SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED;
	}

	if (options > 0)
	{
		return SCE_NP_TROPHY_ERROR_NOT_SUPPORTED;
	}

	trophy_manager.is_initialized = true;

	return CELL_OK;
}

error_code sceNpTrophyTerm()
{
	sceNpTrophy.warning("sceNpTrophyTerm()");

	auto& trophy_manager = g_fxo->get<sce_np_trophy_manager>();

	std::scoped_lock lock(trophy_manager.mtx);

	if (!trophy_manager.is_initialized)
	{
		return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
	}

	idm::clear<trophy_context_t>();
	idm::clear<trophy_handle_t>();

	trophy_manager.is_initialized = false;

	return CELL_OK;
}

error_code sceNpTrophyCreateHandle(vm::ptr<u32> handle)
{
	sceNpTrophy.warning("sceNpTrophyCreateHandle(handle=*0x%x)", handle);

	auto& trophy_manager = g_fxo->get<sce_np_trophy_manager>();

	std::scoped_lock lock(trophy_manager.mtx);

	if (!trophy_manager.is_initialized)
	{
		return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
	}

	if (!handle)
	{
		return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
	}

	const u32 id = idm::make<trophy_handle_t>();

	if (!id)
	{
		return SCE_NP_TROPHY_ERROR_EXCEEDS_MAX;
	}

	*handle = id;
	return CELL_OK;
}

error_code sceNpTrophyDestroyHandle(u32 handle)
{
	sceNpTrophy.warning("sceNpTrophyDestroyHandle(handle=0x%x)", handle);

	auto& trophy_manager = g_fxo->get<sce_np_trophy_manager>();

	std::scoped_lock lock(trophy_manager.mtx);

	// TODO: find out if this is checked
	//if (!trophy_manager.is_initialized)
	//{
	//	return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
	//}

	if (handle < trophy_handle_t::id_base ||
		handle >= trophy_handle_t::id_base + trophy_handle_t::id_count)
	{
		return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
	}

	if (!idm::remove<trophy_handle_t>(handle))
	{
		return SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE;
	}

	return CELL_OK;
}

error_code sceNpTrophyGetGameDetails()
{
	UNIMPLEMENTED_FUNC(sceNpTrophy);
	return CELL_OK;
}

error_code sceNpTrophyAbortHandle(u32 handle)
{
	sceNpTrophy.todo("sceNpTrophyAbortHandle(handle=0x%x)", handle);

	auto& trophy_manager = g_fxo->get<sce_np_trophy_manager>();

	std::scoped_lock lock(trophy_manager.mtx);

	// TODO: find out if this is checked
	//if (!trophy_manager.is_initialized)
	//{
	//	return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
	//}

	if (handle < trophy_handle_t::id_base ||
		handle >= trophy_handle_t::id_base + trophy_handle_t::id_count)
	{
		return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
	}

	const auto hndl = idm::check_unlocked<trophy_handle_t>(handle);

	if (!hndl)
	{
		return SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE;
	}

	// Once it is aborted it cannot be used anymore
	// TODO: Implement function abortion process maybe? (depends if its actually make sense for some functions)
	hndl->is_aborted = true;
	return CELL_OK;
}

error_code sceNpTrophyCreateContext(vm::ptr<u32> context, vm::cptr<SceNpCommunicationId> commId, vm::cptr<SceNpCommunicationSignature> commSign, u64 options)
{
	sceNpTrophy.warning("sceNpTrophyCreateContext(context=*0x%x, commId=*0x%x, commSign=*0x%x, options=0x%llx)", context, commId, commSign, options);

	if (!commSign)
	{
		return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
	}

	sceNpTrophy.notice("sceNpTrophyCreateContext(): commSign = %s", *commSign);

	auto& trophy_manager = g_fxo->get<sce_np_trophy_manager>();

	std::scoped_lock lock(trophy_manager.mtx);

	if (!trophy_manager.is_initialized)
	{
		return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
	}

	if (!context || !commId)
	{
		return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
	}

	if (options > SCE_NP_TROPHY_OPTIONS_CREATE_CONTEXT_READ_ONLY)
	{
		return SCE_NP_TROPHY_ERROR_NOT_SUPPORTED;
	}

	sceNpTrophy.warning("sceNpTrophyCreateContext(): commId = %s", *commId);

	// rough checks for further fmt::format call
	const s32 comm_num = commId->num;
	if (comm_num > 99)
	{
		return SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID;
	}

	// NOTE: commId->term is unused in our code (at least until someone finds out if we need to account for it)

	// Generate trophy context name, limited to 9 characters
	// Read once for thread-safety reasons
	std::string name_str(commId->data, 9);

	// resize the name if it was shorter than expected
	if (const auto pos = name_str.find_first_of('\0'); pos != std::string_view::npos)
	{
		name_str = name_str.substr(0, pos);
	}

	const SceNpCommunicationSignature commSign_data = *commSign;

	if (read_from_ptr<be_t<u32>>(commSign_data.data, 0) != NP_TROPHY_COMM_SIGN_MAGIC)
	{
		return SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID;
	}

	if (std::any_of(&commSign_data.data[6], &commSign_data.data[6] + 6, FN(x != '\0')))
	{
		// 6 padding bytes - must be 0
		return SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID;
	}

	if (read_from_ptr<be_t<u16>>(commSign_data.data, 4) != 0x100)
	{
		// Signifies version (1.00), although only one constant is allowed
		return SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID;
	}

	// append the commId number as "_xx"
	std::string name = fmt::format("%s_%02d", name_str, comm_num);

	// create trophy context
	const auto ctxt = idm::make_ptr<trophy_context_t>();

	if (!ctxt)
	{
		return SCE_NP_TROPHY_ERROR_EXCEEDS_MAX;
	}

	// set trophy context parameters (could be passed to constructor through make_ptr call)
	ctxt->trp_name = name;
	ctxt->comm_id  = *commId;  // stored for RPCN trophy sync/unlock
	ctxt->read_only = !!(options & SCE_NP_TROPHY_OPTIONS_CREATE_CONTEXT_READ_ONLY);
	*context = idm::last_id();

	// set current trophy name for trophy list overlay
	if (!ctxt->read_only)
	{
		current_trophy_name& current_id = g_fxo->get<current_trophy_name>();
		std::lock_guard lock(current_id.mtx);
		current_id.name = std::move(name);
	}

	return CELL_OK;
}

error_code sceNpTrophyDestroyContext(u32 context)
{
	sceNpTrophy.warning("sceNpTrophyDestroyContext(context=0x%x)", context);

	auto& trophy_manager = g_fxo->get<sce_np_trophy_manager>();

	std::scoped_lock lock(trophy_manager.mtx);

	if (!trophy_manager.is_initialized)
	{
		return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
	}

	if (context < trophy_context_t::id_base	||
		context >= trophy_context_t::id_base + trophy_context_t::id_count)
	{
		return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
	}

	if (!idm::remove<trophy_context_t>(context))
	{
		return SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT;
	}

	return CELL_OK;
}

struct register_context_thread_name
{
	static constexpr std::string_view thread_name = "Trophy Register Thread";
};

error_code sceNpTrophyRegisterContext(ppu_thread& ppu, u32 context, u32 handle, vm::ptr<SceNpTrophyStatusCallback> statusCb, vm::ptr<void> arg, u64 options)
{
	sceNpTrophy.warning("sceNpTrophyRegisterContext(context=0x%x, handle=0x%x, statusCb=*0x%x, arg=*0x%x, options=0x%llx)", context, handle, statusCb, arg, options);

	auto& trophy_manager = g_fxo->get<sce_np_trophy_manager>();

	std::shared_lock lock(trophy_manager.mtx);

	if (!trophy_manager.is_initialized)
	{
		return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
	}

	const auto [ctxt, error] = trophy_manager.get_context_ex(context, handle, true);
	const auto handle_ptr = idm::get_unlocked<trophy_handle_t>(handle);

	if (error)
	{
		return error;
	}

	if (!statusCb)
	{
		return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
	}

	if (options > SCE_NP_TROPHY_OPTIONS_REGISTER_CONTEXT_SHOW_ERROR_EXIT)
	{
		return SCE_NP_TROPHY_ERROR_NOT_SUPPORTED;
	}

	const auto on_error = [options]()
	{
		if (!!(options & SCE_NP_TROPHY_OPTIONS_REGISTER_CONTEXT_SHOW_ERROR_EXIT))
		{
			static_cast<void>(open_exit_dialog("Error during trophy registration! The game will now be terminated.", true, msg_dialog_source::_sceNpTrophy));
		}
	};

	// open trophy pack file
	std::string trp_path = vfs::get(Emu.GetDir() + "TROPDIR/" + ctxt->trp_name + "/TROPHY.TRP");
	fs::file stream(trp_path);

	if (!stream && Emu.GetCat() == "GD")
	{
		sceNpTrophy.warning("sceNpTrophyRegisterContext failed to open trophy file from boot path: '%s' (%s)", trp_path, fs::g_tls_error);
		trp_path = vfs::get("/dev_bdvd/PS3_GAME/TROPDIR/" + ctxt->trp_name + "/TROPHY.TRP");
		stream.open(trp_path);
	}

	// check if exists and opened
	if (!stream)
	{
		const std::string msg = fmt::format("Failed to open trophy file: '%s' (%s)", trp_path, fs::g_tls_error);
		return {SCE_NP_TROPHY_ERROR_CONF_DOES_NOT_EXIST, msg};
	}

	// TODO:
	// SCE_NP_TROPHY_STATUS_DATA_CORRUPT     -> reinstall
	// SCE_NP_TROPHY_STATUS_REQUIRES_UPDATE  -> reinstall (for example if a patch has updates for the trophy data)
	// SCE_NP_TROPHY_STATUS_CHANGES_DETECTED -> reinstall (only possible in dev mode)

	const std::string trophyPath = "/dev_hdd0/home/" + Emu.GetUsr() + "/trophy/" + ctxt->trp_name;
	const s32 trp_status = fs::is_dir(vfs::get(trophyPath)) ? SCE_NP_TROPHY_STATUS_INSTALLED : SCE_NP_TROPHY_STATUS_NOT_INSTALLED;

	lock.unlock();

	sceNpTrophy.notice("sceNpTrophyRegisterContext(): Callback is being called (trp_status=%u)", trp_status);

	// "Ask permission" to install the trophy data.
	// The callback is called once and then if it returns >= 0 the cb is called through events(coming from vsh) that are passed to the CB through cellSysutilCheckCallback
	if (statusCb(ppu, context, trp_status, 0, 0, arg) < 0)
	{
		on_error();
		return SCE_NP_TROPHY_ERROR_PROCESSING_ABORTED;
	}

	std::unique_lock lock2(trophy_manager.mtx);

	// Rerun error checks, the callback could have changed stuff by calling sceNpTrophy functions internally

	if (!trophy_manager.is_initialized)
	{
		on_error();
		return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
	}

	const auto [ctxt2, error2] = trophy_manager.get_context_ex(context, handle);

	if (error2)
	{
		// Recheck for any errors, such as if AbortHandle was called
		return error2;
	}

	// Paranoid checks: context/handler could have been destroyed and replaced with new ones with the same IDs
	// Return an error for such cases
	if (ctxt2 != ctxt)
	{
		on_error();
		return SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT;
	}

	if (handle_ptr.get() != idm::check_unlocked<trophy_handle_t>(handle))
	{
		on_error();
		return SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE;
	}

	TRPLoader trp(stream);
	if (!trp.LoadHeader())
	{
		sceNpTrophy.error("sceNpTrophyRegisterContext(): Failed to load trophy config header");
		on_error();
		return SCE_NP_TROPHY_ERROR_ILLEGAL_UPDATE;
	}

	// Rename or discard certain entries based on the files found
	const usz kTargetBufferLength = 31;
	char target[kTargetBufferLength + 1]{};
	strcpy_trunc(target, fmt::format("TROP_%02d.SFM", static_cast<s32>(g_cfg.sys.language)));

	if (trp.ContainsEntry(target))
	{
		trp.RemoveEntry("TROPCONF.SFM");
		trp.RemoveEntry("TROP.SFM");
		trp.RenameEntry(target, "TROPCONF.SFM");
	}
	else if (trp.ContainsEntry("TROP.SFM"))
	{
		trp.RemoveEntry("TROPCONF.SFM");
		trp.RenameEntry("TROP.SFM", "TROPCONF.SFM");
	}
	else if (!trp.ContainsEntry("TROPCONF.SFM"))
	{
		sceNpTrophy.error("sceNpTrophyRegisterContext(): Invalid/Incomplete trophy config");
		on_error();
		return SCE_NP_TROPHY_ERROR_ILLEGAL_UPDATE;
	}

	// Discard unnecessary TROP_XX.SFM files
	for (s32 i = 0; i <= 18; i++)
	{
		strcpy_trunc(target, fmt::format("TROP_%02d.SFM", i));
		if (i != g_cfg.sys.language)
		{
			trp.RemoveEntry(target);
		}
	}

	if (!trp.Install(trophyPath))
	{
		sceNpTrophy.error("sceNpTrophyRegisterContext(): Failed to install trophy context '%s' (%s)", trophyPath, fs::g_tls_error);
		on_error();
		return SCE_NP_TROPHY_ERROR_ILLEGAL_UPDATE;
	}

	const auto& tropusr = ctxt->tropusr = std::make_unique<TROPUSRLoader>();
	const std::string trophyUsrPath = trophyPath + "/TROPUSR.DAT";
	const std::string trophyConfPath = trophyPath + "/TROPCONF.SFM";

	ensure(tropusr->Load(trophyUsrPath, trophyConfPath).success);

	// (Re)load the optional custom trophy rules that belong to this trophy set
	const ct_rules_set custom_rules = ct_get_rules(ctxt->trp_name, true);

	if (g_cfg.net.psn_status == np_psn_status::psn_rpcn)
	{
		const SceNpCommunicationId ctx_comm_id = ctxt->comm_id;
		const u32 trophy_count = tropusr->GetTrophiesCount();

		std::vector<std::pair<s32, s64>> local_unlocked;
		local_unlocked.reserve(trophy_count);
		for (u32 i = 0; i < trophy_count; i++)
		{
			// Custom trophies (and a platinum from a modified set) are never reported to the server
			if (custom_rules.is_custom(static_cast<s32>(i)) || custom_rules.is_removed(static_cast<s32>(i)))
				continue;

			if (custom_rules.active() && tropusr->GetTrophyGrade(i) == SCE_NP_TROPHY_GRADE_PLATINUM)
				continue;

			if (tropusr->GetTrophyUnlockState(static_cast<s32>(i)))
			{
				local_unlocked.emplace_back(
					static_cast<s32>(i),
					static_cast<s64>(tropusr->GetTrophyTimestamp(static_cast<s32>(i))));
			}
		}

		// Release lock before the blocking network call
		lock2.unlock();

		auto& np = g_fxo->get<named_thread<np::np_handler>>();
		std::vector<std::pair<s32, s64>> srv_trophies = np.rpcn_trophy_sync(ctx_comm_id, local_unlocked);

		bool changed = false;
		for (const auto& [tid, ts] : srv_trophies)
		{
			if (tid >= 0 && tid < static_cast<s32>(trophy_count) && ts >= 0 && !tropusr->GetTrophyUnlockState(tid) && !custom_rules.is_custom(tid) && !custom_rules.is_removed(tid))
			{
				static_cast<void>(tropusr->UnlockTrophy(tid, static_cast<u64>(ts), static_cast<u64>(ts)));
				changed = true;
			}
		}

		if (changed && !tropusr->Save(trophyUsrPath))
			sceNpTrophy.error("sceNpTrophyRegisterContext(): Failed to save trophy data after RPCN sync");
	}
	else
	{
		lock2.unlock();
	}

	lv2_obj::sleep(ppu);
	{
		const s32 progress_cb_count = ::narrow<s32>(tropusr->GetTrophiesCount()) - 1;
		{
			// This emulates vsh sending the events and ensures that not 2 events are processed at once
			const std::pair<SceNpTrophyStatus, s32> statuses[] =
			{
				{ SCE_NP_TROPHY_STATUS_PROCESSING_SETUP, 3 },
				{ SCE_NP_TROPHY_STATUS_PROCESSING_PROGRESS, progress_cb_count },
				{ SCE_NP_TROPHY_STATUS_PROCESSING_FINALIZE, std::max<s32>(progress_cb_count, 9) - 5 }, // Seems varying, little bit less than progress_cb_count
				{ SCE_NP_TROPHY_STATUS_PROCESSING_COMPLETE, 0 }
			};

			// Create a counter which is destroyed after the function ends
			const auto queued = std::make_shared<atomic_t<u32>>(0);

			u32 total_events = 0;

			for (auto status : statuses)
			{
				total_events += status.second + 1;
			}

			for (auto status : statuses)
			{
				for (s32 completed = 0; completed <= status.second; completed++)
				{
					// One status max per cellSysutilCheckCallback call
					*queued += 1;

					sysutil_register_cb([statusCb, status, context, completed, arg, queued](ppu_thread& cb_ppu) -> s32
					{
						// TODO: it is possible that we need to check the return value here as well.
						statusCb(cb_ppu, context, status.first, completed, status.second, arg);

						if (queued && (*queued)-- == 1)
						{
							queued->notify_one();
						}

						return 0;
					});

					u64 current = get_system_time();

					// Minimum register trophy time 2 seconds globally.
					const u64 until_min = current + (2'000'000 / total_events);
					const u64 until_max = until_min + 50'000;

					// If too much time passes just send the rest of the events anyway
					for (u32 old_value = *queued; current < (old_value ? until_max : until_min);
						current = get_system_time(), old_value = *queued)
					{
						if (!old_value)
						{
							thread_ctrl::wait_for(until_min - current);
						}
						else
						{
							thread_ctrl::wait_on(*queued, old_value, until_max - current);
						}

						if (thread_ctrl::state() == thread_state::aborting)
						{
							return {};
						}
					}
				}
			}
		}
	}

	return CELL_OK;
}

error_code sceNpTrophyGetRequiredDiskSpace(u32 context, u32 handle, vm::ptr<u64> reqspace, u64 options)
{
	sceNpTrophy.warning("sceNpTrophyGetRequiredDiskSpace(context=0x%x, handle=0x%x, reqspace=*0x%x, options=0x%llx)", context, handle, reqspace, options);

	if (!reqspace)
	{
		return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
	}

	if (options > 0)
	{
		return SCE_NP_TROPHY_ERROR_NOT_SUPPORTED;
	}

	auto& trophy_manager = g_fxo->get<sce_np_trophy_manager>();

	reader_lock lock(trophy_manager.mtx);

	if (!trophy_manager.is_initialized)
	{
		return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
	}

	const auto [ctxt, error] = trophy_manager.get_context_ex(context, handle);

	if (error)
	{
		return error;
	}

	u64 space = 0;

	if (!fs::is_dir(vfs::get("/dev_hdd0/home/" + Emu.GetUsr() + "/trophy/" + ctxt->trp_name)))
	{
		// open trophy pack file
		std::string trophy_path = vfs::get(Emu.GetDir() + "TROPDIR/" + ctxt->trp_name + "/TROPHY.TRP");
		fs::file stream(trophy_path);

		if (!stream && Emu.GetCat() == "GD")
		{
			sceNpTrophy.warning("sceNpTrophyGetRequiredDiskSpace failed to open trophy file from boot path: '%s'", trophy_path);
			trophy_path = vfs::get("/dev_bdvd/PS3_GAME/TROPDIR/" + ctxt->trp_name + "/TROPHY.TRP");
			stream.open(trophy_path);
		}

		// check if exists and opened
		if (!stream)
		{
			return {SCE_NP_TROPHY_ERROR_CONF_DOES_NOT_EXIST, trophy_path};
		}

		TRPLoader trp(stream);

		if (trp.LoadHeader())
		{
			space = trp.GetRequiredSpace();
		}
		else
		{
			sceNpTrophy.error("sceNpTrophyGetRequiredDiskSpace(): Failed to load trophy header! (trp_name=%s)", ctxt->trp_name);
		}
	}
	else
	{
		sceNpTrophy.warning("sceNpTrophyGetRequiredDiskSpace(): Trophy config is already installed (trp_name=%s)", ctxt->trp_name);
	}

	sceNpTrophy.warning("sceNpTrophyGetRequiredDiskSpace(): reqspace is 0x%llx", space);

	*reqspace = space;
	return CELL_OK;
}

error_code sceNpTrophySetSoundLevel(u32 context, u32 handle, u32 level, u64 options)
{
	sceNpTrophy.todo("sceNpTrophySetSoundLevel(context=0x%x, handle=0x%x, level=%d, options=0x%llx)", context, handle, level, options);

	if (level > 100 || level < 20)
	{
		return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
	}

	if (options > 0)
	{
		return SCE_NP_TROPHY_ERROR_NOT_SUPPORTED;
	}

	auto& trophy_manager = g_fxo->get<sce_np_trophy_manager>();

	reader_lock lock(trophy_manager.mtx);

	if (!trophy_manager.is_initialized)
	{
		return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
	}

	const auto [ctxt, error] = trophy_manager.get_context_ex(context, handle);

	if (error)
	{
		return error;
	}

	return CELL_OK;
}

error_code sceNpTrophyGetGameInfo(u32 context, u32 handle, vm::ptr<SceNpTrophyGameDetails> details, vm::ptr<SceNpTrophyGameData> data)
{
	sceNpTrophy.warning("sceNpTrophyGetGameInfo(context=0x%x, handle=0x%x, details=*0x%x, data=*0x%x)", context, handle, details, data);

	auto& trophy_manager = g_fxo->get<sce_np_trophy_manager>();

	reader_lock lock(trophy_manager.mtx);

	if (!trophy_manager.is_initialized)
	{
		return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
	}

	const auto [ctxt, error] = trophy_manager.get_context_ex(context, handle);

	if (error)
	{
		return error;
	}

	if (!ctxt->tropusr)
	{
		// TODO: May return SCE_NP_TROPHY_ERROR_UNKNOWN_TITLE for older sdk version
		return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;
	}

	if (!details && !data)
	{
		return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
	}

	const std::string config_path = vfs::get("/dev_hdd0/home/" + Emu.GetUsr() + "/trophy/" + ctxt->trp_name + "/TROPCONF.SFM");

	fs::file config(config_path);

	if (!config)
	{
		return { SCE_NP_TROPHY_ERROR_CONF_DOES_NOT_EXIST, config_path };
	}

	if (details)
		*details = {};
	if (data)
		*data = {};

	trophy_xml_document doc{};
	pugi::xml_parse_result res = doc.Read(config.to_string());
	if (!res)
	{
		sceNpTrophy.error("sceNpTrophyGetGameInfo: Failed to read TROPCONF.SFM: %s", config_path);
		// TODO: return some error
		return CELL_OK;
	}

	std::shared_ptr<rXmlNode> trophy_base = doc.GetRoot();
	if (!trophy_base)
	{
		sceNpTrophy.error("sceNpTrophyGetGameInfo: Failed to read TROPCONF.SFM (root is null): %s", config_path);
		// TODO: return some error
		return CELL_OK;
	}

	for (std::shared_ptr<rXmlNode> n = trophy_base->GetChildren(); n; n = n->GetNext())
	{
		const std::string n_name = n->GetName();

		if (details)
		{
			if (n_name == "title-name")
			{
				strcpy_trunc(details->title, n->GetNodeContent());
				continue;
			}
			else if (n_name == "title-detail")
			{
				strcpy_trunc(details->description, n->GetNodeContent());
				continue;
			}
		}

		if (n_name == "trophy")
		{
			if (details)
			{
				details->numTrophies++;
				switch (n->GetAttribute("ttype")[0])
				{
				case 'B': details->numBronze++;   break;
				case 'S': details->numSilver++;   break;
				case 'G': details->numGold++;     break;
				case 'P': details->numPlatinum++; break;
				}
			}

			if (data)
			{
				const u32 trophy_id = atoi(n->GetAttribute("id").c_str());

				if (ctxt->tropusr->GetTrophyUnlockState(trophy_id))
				{
					data->unlockedTrophies++;
					switch (n->GetAttribute("ttype")[0])
					{
					case 'B': data->unlockedBronze++;   break;
					case 'S': data->unlockedSilver++;   break;
					case 'G': data->unlockedGold++;     break;
					case 'P': data->unlockedPlatinum++; break;
					default: break;
					}
				}
			}
		}
	}

	return CELL_OK;
}

error_code sceNpTrophyGetLatestTrophies()
{
	UNIMPLEMENTED_FUNC(sceNpTrophy);
	return CELL_OK;
}

error_code sceNpTrophyUnlockTrophy(ppu_thread& ppu, u32 context, u32 handle, s32 trophyId, vm::ptr<u32> platinumId)
{
	sceNpTrophy.warning("sceNpTrophyUnlockTrophy(context=0x%x, handle=0x%x, trophyId=%d, platinumId=*0x%x)", context, handle, trophyId, platinumId);

	auto& trophy_manager = g_fxo->get<sce_np_trophy_manager>();

	std::scoped_lock lock(trophy_manager.mtx);

	if (!trophy_manager.is_initialized)
	{
		return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
	}

	const auto [ctxt, error] = trophy_manager.get_context_ex(context, handle, true);

	if (error)
	{
		return error;
	}

	if (!ctxt->tropusr)
	{
		// TODO: May return SCE_NP_TROPHY_ERROR_UNKNOWN_TITLE for older sdk version
		return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;
	}

	if (trophyId < 0 || trophyId >= static_cast<s32>(ctxt->tropusr->GetTrophiesCount()))
	{
		return SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID;
	}

	if (ctxt->tropusr->GetTrophyGrade(trophyId) == SCE_NP_TROPHY_GRADE_PLATINUM)
	{
		return SCE_NP_TROPHY_ERROR_CANNOT_UNLOCK_PLATINUM;
	}

	if (ctxt->tropusr->GetTrophyUnlockState(trophyId))
	{
		return SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED;
	}

	const ct_rules_set& rules = ct_get_rules(ctxt->trp_name);

	if (rules.is_removed(trophyId))
	{
		// Removed by the custom trophy rules: pretend it worked, but only re-check the platinum
		sceNpTrophy.notice("sceNpTrophyUnlockTrophy: trophy %d is removed by the custom trophy rules, ignoring", trophyId);

		const u32 platinum = np_trophy_unlock_impl(ppu, ctxt, rules, trophyId, false);

		if (platinumId)
		{
			*platinumId = platinum;
		}

		return CELL_OK;
	}

	const u32 unlocked_platinum_id = np_trophy_unlock_impl(ppu, ctxt, rules, trophyId, true);

	if (platinumId)
	{
		*platinumId = unlocked_platinum_id;
		sceNpTrophy.warning("sceNpTrophyUnlockTrophy: platinumId was set to %d", unlocked_platinum_id);
	}

	return CELL_OK;
}

error_code sceNpTrophyGetTrophyUnlockState(u32 context, u32 handle, vm::ptr<SceNpTrophyFlagArray> flags, vm::ptr<u32> count)
{
	sceNpTrophy.warning("sceNpTrophyGetTrophyUnlockState(context=0x%x, handle=0x%x, flags=*0x%x, count=*0x%x)", context, handle, flags, count);

	if (!flags || !count)
	{
		return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
	}

	auto& trophy_manager = g_fxo->get<sce_np_trophy_manager>();

	reader_lock lock(trophy_manager.mtx);

	if (!trophy_manager.is_initialized)
	{
		return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
	}

	const auto [ctxt, error] = trophy_manager.get_context_ex(context, handle);

	if (error)
	{
		return error;
	}

	TROPUSRLoader* tropusr = nullptr;
	TROPUSRLoader local_tropusr{};

	if (ctxt->read_only)
	{
		const std::string trophyPath = "/dev_hdd0/home/" + Emu.GetUsr() + "/trophy/" + ctxt->trp_name;
		const std::string trophyUsrPath = trophyPath + "/TROPUSR.DAT";
		const std::string trophyConfPath = trophyPath + "/TROPCONF.SFM";

		if (local_tropusr.Load(trophyUsrPath, trophyConfPath).success)
		{
			tropusr = &local_tropusr;
		}
		else
		{
			// TODO: confirm
			*count = 0;
			*flags = {};
			return CELL_OK;
		}
	}
	else
	{
		if (!ctxt->tropusr)
		{
			// TODO: May return SCE_NP_TROPHY_ERROR_UNKNOWN_TITLE for older sdk version
			return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;
		}

		tropusr = ctxt->tropusr.get();
	}

	ensure(tropusr);

	u32 count_ = tropusr->GetTrophiesCount();
	*count = count_;

	if (count_ > 128)
	{
		sceNpTrophy.error("sceNpTrophyGetTrophyUnlockState: More than 128 trophies detected! (count=%d)", count_);
		count_ = 128;
	}

	// Needs hw testing
	*flags = {};

	// Pack up to 128 bools in u32 flag_bits[4]
	for (u32 id = 0; id < count_; id++)
	{
		if (tropusr->GetTrophyUnlockState(id))
			flags->flag_bits[id / 32] |= 1u << (id % 32);
		else
			flags->flag_bits[id / 32] &= ~(1u << (id % 32));
	}

	return CELL_OK;
}

error_code sceNpTrophyGetTrophyDetails()
{
	UNIMPLEMENTED_FUNC(sceNpTrophy);
	return CELL_OK;
}

static error_code NpTrophyGetTrophyInfo(const trophy_context_t* ctxt, s32 trophyId, SceNpTrophyDetails* details, SceNpTrophyData* data)
{
	if (!details && !data)
	{
		return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
	}

	if (!ctxt->tropusr)
	{
		// TODO: May return SCE_NP_TROPHY_ERROR_UNKNOWN_TITLE for older sdk version
		return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;
	}

	const std::string config_path = vfs::get("/dev_hdd0/home/" + Emu.GetUsr() + "/trophy/" + ctxt->trp_name + "/TROPCONF.SFM");

	fs::file config(config_path);

	if (!config)
	{
		return { SCE_NP_TROPHY_ERROR_CONF_DOES_NOT_EXIST, config_path };
	}

	SceNpTrophyDetails tmp_details{};
	SceNpTrophyData tmp_data{};

	trophy_xml_document doc{};
	pugi::xml_parse_result res = doc.Read(config.to_string());
	if (!res)
	{
		sceNpTrophy.error("sceNpTrophyGetGameInfo: Failed to read TROPCONF.SFM: %s", config_path);
		// TODO: return some error
	}

	auto trophy_base = doc.GetRoot();
	if (!trophy_base)
	{
		sceNpTrophy.error("sceNpTrophyGetGameInfo: Failed to read TROPCONF.SFM (root is null): %s", config_path);
		// TODO: return some error
	}

	bool found = false;
	for (std::shared_ptr<rXmlNode> n = trophy_base ? trophy_base->GetChildren() : nullptr; n; n = n->GetNext())
	{
		if (n->GetName() == "trophy" && (trophyId == atoi(n->GetAttribute("id").c_str())))
		{
			found = true;

			const bool hidden = n->GetAttribute("hidden")[0] == 'y';
			const bool unlocked = !!ctxt->tropusr->GetTrophyUnlockState(trophyId);

			if (hidden && !unlocked) // Trophy is hidden
			{
				return SCE_NP_TROPHY_ERROR_HIDDEN;
			}

			if (details)
			{
				tmp_details.trophyId = trophyId;
				tmp_details.hidden = hidden;

				switch (n->GetAttribute("ttype")[0])
				{
				case 'B': tmp_details.trophyGrade = SCE_NP_TROPHY_GRADE_BRONZE;   break;
				case 'S': tmp_details.trophyGrade = SCE_NP_TROPHY_GRADE_SILVER;   break;
				case 'G': tmp_details.trophyGrade = SCE_NP_TROPHY_GRADE_GOLD;     break;
				case 'P': tmp_details.trophyGrade = SCE_NP_TROPHY_GRADE_PLATINUM; break;
				default: break;
				}

				for (std::shared_ptr<rXmlNode> n2 = n->GetChildren(); n2; n2 = n2->GetNext())
				{
					const std::string n2_name = n2->GetName();

					if (n2_name == "name")
					{
						strcpy_trunc(tmp_details.name, n2->GetNodeContent());
					}
					else if (n2_name == "detail")
					{
						strcpy_trunc(tmp_details.description, n2->GetNodeContent());
					}
				}
			}

			if (data)
			{
				tmp_data.trophyId = trophyId;
				tmp_data.unlocked = unlocked;
				tmp_data.timestamp = ctxt->tropusr->GetTrophyTimestamp(trophyId);
			}

			break;
		}
	}

	if (!found)
	{
		return SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID;
	}

	if (details)
	{
		*details = tmp_details;
	}

	if (data)
	{
		*data = tmp_data;
	}

	return CELL_OK;
}

error_code sceNpTrophyGetTrophyInfo(u32 context, u32 handle, s32 trophyId, vm::ptr<SceNpTrophyDetails> details, vm::ptr<SceNpTrophyData> data)
{
	sceNpTrophy.warning("sceNpTrophyGetTrophyInfo(context=0x%x, handle=0x%x, trophyId=%d, details=*0x%x, data=*0x%x)", context, handle, trophyId, details, data);

	if (trophyId < 0 || trophyId > 127) // max 128 trophies
	{
		return SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID;
	}

	auto& trophy_manager = g_fxo->get<sce_np_trophy_manager>();

	reader_lock lock(trophy_manager.mtx);

	if (!trophy_manager.is_initialized)
	{
		return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
	}

	const auto [ctxt, error] = trophy_manager.get_context_ex(context, handle);

	if (error)
	{
		return error;
	}

	return NpTrophyGetTrophyInfo(ctxt, trophyId, details ? details.get_ptr() : nullptr, data ? data.get_ptr() : nullptr);
}

error_code sceNpTrophyGetGameProgress(u32 context, u32 handle, vm::ptr<s32> percentage)
{
	sceNpTrophy.warning("sceNpTrophyGetGameProgress(context=0x%x, handle=0x%x, percentage=*0x%x)", context, handle, percentage);

	if (!percentage)
	{
		return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
	}

	auto& trophy_manager = g_fxo->get<sce_np_trophy_manager>();

	reader_lock lock(trophy_manager.mtx);

	if (!trophy_manager.is_initialized)
	{
		return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
	}

	const auto [ctxt, error] = trophy_manager.get_context_ex(context, handle);

	if (error)
	{
		return error;
	}

	if (!ctxt->tropusr)
	{
		// TODO: May return SCE_NP_TROPHY_ERROR_UNKNOWN_TITLE for older sdk version
		return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;
	}

	const u32 unlocked = ctxt->tropusr->GetUnlockedTrophiesCount();
	const u32 trp_count = ctxt->tropusr->GetTrophiesCount();

	// Round result to nearest (TODO: Check 0 trophies)
	*percentage = trp_count ? utils::rounded_div(unlocked * 100, trp_count) : 0;

	if (trp_count == 0 || trp_count > 128)
	{
		sceNpTrophy.warning("sceNpTrophyGetGameProgress(): Trophies count may be invalid or untested (%d)", trp_count);
	}

	return CELL_OK;
}

error_code sceNpTrophyGetGameIcon(u32 context, u32 handle, vm::ptr<void> buffer, vm::ptr<u32> size)
{
	sceNpTrophy.warning("sceNpTrophyGetGameIcon(context=0x%x, handle=0x%x, buffer=*0x%x, size=*0x%x)", context, handle, buffer, size);

	auto& trophy_manager = g_fxo->get<sce_np_trophy_manager>();

	reader_lock lock(trophy_manager.mtx);

	if (!trophy_manager.is_initialized)
	{
		return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
	}

	const auto [ctxt, error] = trophy_manager.get_context_ex(context, handle);

	if (error)
	{
		return error;
	}

	if (!size)
	{
		return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
	}

	// Try to get icon in current language first
	const std::string trophy_path = fmt::format("/dev_hdd0/home/%s/trophy/%s/", Emu.GetUsr(), ctxt->trp_name);
	fs::file icon_file(vfs::get(fmt::format("%s/ICON0_%02d.PNG", trophy_path, static_cast<s32>(g_cfg.sys.language))));

	if (!icon_file && !icon_file.open(vfs::get(fmt::format("%s/ICON0.PNG", trophy_path))))
	{
		return SCE_NP_TROPHY_ERROR_UNKNOWN_FILE;
	}

	const u32 icon_size = ::size32(icon_file);

	if (buffer && *size >= icon_size)
	{
		lv2_file::op_read(icon_file, buffer, icon_size);
	}

	*size = icon_size;

	return CELL_OK;
}

error_code sceNpTrophyGetUserInfo()
{
	UNIMPLEMENTED_FUNC(sceNpTrophy);
	return CELL_OK;
}

error_code sceNpTrophyGetTrophyIcon(u32 context, u32 handle, s32 trophyId, vm::ptr<void> buffer, vm::ptr<u32> size)
{
	sceNpTrophy.warning("sceNpTrophyGetTrophyIcon(context=0x%x, handle=0x%x, trophyId=%d, buffer=*0x%x, size=*0x%x)", context, handle, trophyId, buffer, size);

	auto& trophy_manager = g_fxo->get<sce_np_trophy_manager>();

	reader_lock lock(trophy_manager.mtx);

	if (!trophy_manager.is_initialized)
	{
		return SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
	}

	const auto [ctxt, error] = trophy_manager.get_context_ex(context, handle);

	if (error)
	{
		return error;
	}

	if (!size)
	{
		return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
	}

	if (!ctxt->tropusr)
	{
		// TODO: May return SCE_NP_TROPHY_ERROR_UNKNOWN_TITLE for older sdk version
		return SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;
	}

	if (ctxt->tropusr->GetTrophiesCount() <= static_cast<u32>(trophyId))
	{
		return SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID;
	}

	if (!ctxt->tropusr->GetTrophyUnlockState(trophyId))
	{
		const std::string config_path = vfs::get("/dev_hdd0/home/" + Emu.GetUsr() + "/trophy/" + ctxt->trp_name + "/TROPCONF.SFM");

		fs::file config(config_path);
		if (config)
		{
			trophy_xml_document doc{};
			pugi::xml_parse_result res = doc.Read(config.to_string());
			if (!res)
			{
				sceNpTrophy.error("sceNpTrophyGetTrophyIcon: Failed to read TROPCONF.SFM: %s", config_path);
				// TODO: return some error
			}

			auto trophy_base = doc.GetRoot();
			if (!trophy_base)
			{
				sceNpTrophy.error("sceNpTrophyGetTrophyIcon: Failed to read TROPCONF.SFM (root is null): %s", config_path);
				// TODO: return some error
			}

			for (std::shared_ptr<rXmlNode> n = trophy_base ? trophy_base->GetChildren() : nullptr; n; n = n->GetNext())
			{
				if (n->GetName() == "trophy" && trophyId == atoi(n->GetAttribute("id").c_str()) && n->GetAttribute("hidden")[0] == 'y')
				{
					return SCE_NP_TROPHY_ERROR_HIDDEN;
				}
			}
		}
		else
		{
			// TODO: Maybe return SCE_NP_TROPHY_ERROR_CONF_DOES_NOT_EXIST
		}

		return SCE_NP_TROPHY_ERROR_LOCKED;
	}

	fs::file icon_file(vfs::get("/dev_hdd0/home/" + Emu.GetUsr() + "/trophy/" + ctxt->trp_name + fmt::format("/TROP%03d.PNG", trophyId)));

	if (!icon_file)
	{
		return SCE_NP_TROPHY_ERROR_UNKNOWN_FILE;
	}

	const u32 icon_size = ::size32(icon_file);

	if (buffer && *size >= icon_size)
	{
		lv2_file::op_read(icon_file, buffer, icon_size);
	}

	*size = icon_size;

	return CELL_OK;
}

error_code sceNpTrophyNetworkSync()
{
	UNIMPLEMENTED_FUNC(sceNpTrophy);
	return CELL_OK;
}

DECLARE(ppu_module_manager::sceNpTrophy)("sceNpTrophy", []()
{
	REG_FUNC(sceNpTrophy, sceNpTrophyGetGameProgress);
	REG_FUNC(sceNpTrophy, sceNpTrophyRegisterContext);
	REG_FUNC(sceNpTrophy, sceNpTrophyCreateHandle);
	REG_FUNC(sceNpTrophy, sceNpTrophySetSoundLevel);
	REG_FUNC(sceNpTrophy, sceNpTrophyGetRequiredDiskSpace);
	REG_FUNC(sceNpTrophy, sceNpTrophyDestroyContext);
	REG_FUNC(sceNpTrophy, sceNpTrophyInit);
	REG_FUNC(sceNpTrophy, sceNpTrophyAbortHandle);
	REG_FUNC(sceNpTrophy, sceNpTrophyGetGameInfo);
	REG_FUNC(sceNpTrophy, sceNpTrophyDestroyHandle);
	REG_FUNC(sceNpTrophy, sceNpTrophyGetGameDetails);
	REG_FUNC(sceNpTrophy, sceNpTrophyUnlockTrophy);
	REG_FUNC(sceNpTrophy, sceNpTrophyGetLatestTrophies);
	REG_FUNC(sceNpTrophy, sceNpTrophyTerm);
	REG_FUNC(sceNpTrophy, sceNpTrophyGetTrophyUnlockState);
	REG_FUNC(sceNpTrophy, sceNpTrophyGetUserInfo);
	REG_FUNC(sceNpTrophy, sceNpTrophyGetTrophyIcon);
	REG_FUNC(sceNpTrophy, sceNpTrophyCreateContext);
	REG_FUNC(sceNpTrophy, sceNpTrophyGetTrophyDetails);
	REG_FUNC(sceNpTrophy, sceNpTrophyGetTrophyInfo);
	REG_FUNC(sceNpTrophy, sceNpTrophyGetGameIcon);
	REG_FUNC(sceNpTrophy, sceNpTrophyNetworkSync);
});
