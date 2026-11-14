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
#include "Emu/Io/pad_types.h"
#include "Input/pad_thread.h"

#include "Emu/Cell/lv2/sys_event.h"
#include "Emu/Cell/lv2/sys_fs.h"

#include <algorithm>
#include <cstdio>
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
//   custom:                      # optional: new trophies, added after the game's own
//     - name: "Pest Control"
//       detail: "Kill 14 enemies"
//       grade: B                 # B, S or G
//       icon: pest.png           # in trophy_rules/<NPWRxxxxx_00>/, optional
//       when: "[[0xF3F20]+0x24] >= 14"
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
	ct_delta,  // value minus what it was when first read this session
	ct_prev,   // value at the previous check
	ct_trophy, // 1 if that trophy id is unlocked, else 0
	ct_cmp,    // comparison inside brackets, gives 1 or 0
	ct_count,  // how many times the value went from zero to non-zero (kept between sessions)
	ct_button, // 1 while that controller button is held
};

struct ct_op
{
	u8 type = ct_push;
	f64 value = 0;
	u8 size = 4;
	bool is_signed = false;
	bool is_float = false;

	// Remembered between checks by ct_delta and ct_prev
	f64 state = 0;
	bool has_state = false;
	bool flag = false;
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
	u64 hold_us = 0; // "for 60s": has to stay true this long

	// State
	u64 held_us = 0;
	f64 last = 0;         // last value of the left side, for progress display
	bool readable = false;
};

using ct_alt = std::vector<ct_condition>; // all of these...
using ct_clause = std::vector<ct_alt>;    // ...for any one alternative ("||")

struct ct_step
{
	std::vector<ct_clause> clauses; // every clause has to hold
};

struct ct_rule
{
	s32 id = -1;
	std::vector<ct_step> steps; // in order; a plain "when" is a single step
	ct_step reset;              // optional: sends a sequence back to its first step
	std::string text;

	// State
	u32 current = 0;
	bool has_progress = false; // simple "value >= target" rules can show progress
	f64 progress_target = 0;
	s64 progress_shown = -1;
};

struct ct_eval_ctx
{
	const TROPUSRLoader* tropusr = nullptr;
	u64 dt_us = 0; // time since the previous check (paused time is not counted)
};

// A trophy the pack adds on top of the game's own set
struct ct_custom_trophy
{
	std::string name;
	std::string detail;
	std::string icon;
	char grade = 'B';
	bool hidden = false;
	bool deleted = false; // keeps its place in the list (so later trophies keep their numbers) but is hidden and cannot be earned
	ct_rule rule; // id is assigned when the trophy is merged in
};

struct ct_rules_set
{
	std::vector<ct_custom_trophy> custom;
	u32 defined_trophies = 0; // official + custom, 0 if unknown
	u64 last_check = 0;
	u64 next_progress_write = 0;
	bool progress_dirty = false;
	bool fast = false;       // has button() rules: checked every frame instead of 4x a second
	bool has_counts = false; // has count() rules: totals are saved to disk
	f64 saved_counts = -1;
	u64 next_counts_write = 0;
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
	std::map<std::string, std::set<s32>> traced; // Trophy ids already written to the trace this session

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

	// expr [compare expr], for use inside brackets: "count([0x1000] > 5)"
	bool cmp_expr()
	{
		if (!expr())
		{
			return false;
		}

		skip();

		const std::string_view rest = s.substr(pos);
		u8 cmp = ct_eq;

		if (rest.starts_with("=="))
		{
			cmp = ct_eq;
			pos += 2;
		}
		else if (rest.starts_with("!="))
		{
			cmp = ct_ne;
			pos += 2;
		}
		else if (rest.starts_with(">="))
		{
			cmp = ct_ge;
			pos += 2;
		}
		else if (rest.starts_with("<="))
		{
			cmp = ct_le;
			pos += 2;
		}
		else if (rest.starts_with(">"))
		{
			cmp = ct_gt;
			pos += 1;
		}
		else if (rest.starts_with("<"))
		{
			cmp = ct_lt;
			pos += 1;
		}
		else
		{
			return true;
		}

		if (!expr())
		{
			return false;
		}

		ct_op op{};
		op.type = ct_cmp;
		op.value = cmp;
		out->push_back(op);
		return true;
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
			return cmp_expr() && eat(')');
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

		// delta(x), prev(x), trophy(n)
		static constexpr std::pair<std::string_view, u8> s_funcs[] =
		{
			{"delta", ct_delta},
			{"prev", ct_prev},
			{"trophy", ct_trophy},
			{"count", ct_count},
			{"button", ct_button},
		};

		for (const auto& [name, type] : s_funcs)
		{
			if (s.substr(pos).starts_with(name))
			{
				usz next = pos + name.size();

				while (next < s.size() && (s[next] == ' ' || s[next] == '\t'))
				{
					next++;
				}

				if (next < s.size() && s[next] == '(')
				{
					pos = next + 1;

					ct_op func{};
					func.type = type;

					if (type == ct_button)
					{
						// button(cross): the argument is a name, not a number
						skip();

						std::string button;

						while (pos < s.size() && ((s[pos] >= 'a' && s[pos] <= 'z') || (s[pos] >= 'A' && s[pos] <= 'Z') || (s[pos] >= '0' && s[pos] <= '9')))
						{
							const char c = s[pos++];
							button += (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
						}

						struct button_name
						{
							std::string_view name;
							u8 offset; // 2 = digital1, 3 = digital2
							u32 code;
						};

						static constexpr button_name s_buttons[] =
						{
							{"select", 2, 0x01}, {"l3", 2, 0x02}, {"r3", 2, 0x04}, {"start", 2, 0x08},
							{"up", 2, 0x10}, {"right", 2, 0x20}, {"down", 2, 0x40}, {"left", 2, 0x80},
							{"l2", 3, 0x01}, {"r2", 3, 0x02}, {"l1", 3, 0x04}, {"r1", 3, 0x08},
							{"triangle", 3, 0x10}, {"circle", 3, 0x20}, {"cross", 3, 0x40}, {"x", 3, 0x40}, {"square", 3, 0x80},
						};

						bool found = false;

						for (const button_name& entry : s_buttons)
						{
							if (button == entry.name)
							{
								func.size = entry.offset;
								func.value = entry.code;
								found = true;
								break;
							}
						}

						if (!found || !eat(')'))
						{
							return false;
						}

						out->push_back(func);
						return true;
					}

					if (!cmp_expr() || !eat(')'))
					{
						return false;
					}

					out->push_back(func);
					return true;
				}
			}
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

	if (!parser.expr())
	{
		return false;
	}

	// Optional "for 60s" / "for 2m": the comparison has to stay true that long
	parser.skip();

	if (const std::string_view tail = parser.s.substr(parser.pos); tail.starts_with("for ") || tail.starts_with("for\t"))
	{
		parser.pos += 3;
		parser.skip();

		std::string number;

		while (parser.pos < parser.s.size() && ((parser.s[parser.pos] >= '0' && parser.s[parser.pos] <= '9') || parser.s[parser.pos] == '.'))
		{
			number += parser.s[parser.pos++];
		}

		char* end = nullptr;
		const f64 amount = number.empty() ? -1.0 : std::strtod(number.c_str(), &end);

		if (amount < 0 || !end || *end != '\0' || amount > 86400.0)
		{
			return false;
		}

		f64 seconds = amount;

		if (parser.eat('m'))
		{
			seconds = parser.eat('s') ? amount / 1000.0 : amount * 60.0;
		}
		else
		{
			parser.eat('s');
		}

		cond.hold_us = static_cast<u64>(seconds * 1'000'000.0);
	}

	return parser.at_end();
}

// Evaluate against guest memory. Fails (false) if anything points at unmapped memory.
static bool ct_eval(std::vector<ct_op>& ops, const ct_eval_ctx& ctx, f64& result)
{
	f64 stack[16]{};
	u32 sp = 0;

	for (ct_op& op : ops)
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
		case ct_button:
		{
			if (sp >= std::size(stack))
			{
				return false;
			}

			bool pressed = false;

			{
				std::lock_guard pad_lock(pad::g_pad_mutex);

				if (const auto handler = pad::get_pad_thread(true))
				{
					if (const auto& pad = handler->GetPads()[0])
					{
						for (const Button& button : pad->m_buttons)
						{
							if (button.m_pressed && button.m_offset == op.size && button.m_outKeyCode == static_cast<u32>(op.value))
							{
								pressed = true;
								break;
							}
						}
					}
				}
			}

			stack[sp++] = pressed ? 1 : 0;
			break;
		}
		case ct_cmp:
		{
			if (sp < 2)
			{
				return false;
			}

			const f64 b = stack[--sp];
			const f64 a = stack[sp - 1];
			bool ok = false;

			switch (static_cast<u8>(op.value))
			{
			case ct_eq: ok = a == b; break;
			case ct_ne: ok = a != b; break;
			case ct_ge: ok = a >= b; break;
			case ct_le: ok = a <= b; break;
			case ct_gt: ok = a > b; break;
			case ct_lt: ok = a < b; break;
			default: break;
			}

			stack[sp - 1] = ok ? 1 : 0;
			break;
		}
		case ct_count:
		{
			if (sp < 1)
			{
				return false;
			}

			const bool truthy = stack[sp - 1] != 0;

			if (truthy && !op.flag)
			{
				op.state += 1;
			}

			op.flag = truthy;
			stack[sp - 1] = op.state;
			break;
		}
		case ct_delta:
		case ct_prev:
		case ct_trophy:
		{
			if (sp < 1)
			{
				return false;
			}

			const f64 a = stack[sp - 1];

			if (op.type == ct_delta)
			{
				if (!op.has_state)
				{
					op.state = a;
					op.has_state = true;
				}

				stack[sp - 1] = a - op.state;
			}
			else if (op.type == ct_prev)
			{
				stack[sp - 1] = op.has_state ? op.state : a;
				op.state = a;
				op.has_state = true;
			}
			else
			{
				const bool unlocked = ctx.tropusr && a >= 0 && a < ctx.tropusr->GetTrophiesCount() && ctx.tropusr->GetTrophyUnlockState(static_cast<u32>(a));
				stack[sp - 1] = unlocked ? 1 : 0;
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

static bool ct_check(ct_condition& cond, const ct_eval_ctx& ctx)
{
	f64 a = 0;
	f64 b = 0;

	// Both sides are always evaluated so that prev() and delta() see every check
	const bool read_a = ct_eval(cond.lhs, ctx, a);
	const bool read_b = ct_eval(cond.rhs, ctx, b);

	cond.readable = read_a;
	cond.last = a;

	bool ok = false;

	if (read_a && read_b)
	{
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
	}

	if (!ok)
	{
		cond.held_us = 0;
		return false;
	}

	if (!cond.hold_us)
	{
		return true;
	}

	cond.held_us += ctx.dt_us;
	return cond.held_us >= cond.hold_us;
}

static bool ct_check_step(ct_step& step, const ct_eval_ctx& ctx)
{
	bool all = !step.clauses.empty();

	for (ct_clause& clause : step.clauses)
	{
		bool any = false;

		for (ct_alt& alt : clause)
		{
			bool alt_ok = !alt.empty();

			for (ct_condition& cond : alt)
			{
				// No early exit, see ct_check
				if (!ct_check(cond, ctx))
				{
					alt_ok = false;
				}
			}

			any |= alt_ok;
		}

		all &= any;
	}

	return all;
}

static bool ct_test(ct_rule& rule, const ct_eval_ctx& ctx)
{
	if (rule.steps.empty())
	{
		return false;
	}

	if (!rule.reset.clauses.empty() && ct_check_step(rule.reset, ctx))
	{
		// Back to the start, and nothing counts while the reset condition holds
		rule.current = 0;
		return false;
	}

	if (rule.current < rule.steps.size() && ct_check_step(rule.steps[rule.current], ctx))
	{
		rule.current++;
	}

	return rule.current >= rule.steps.size();
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

// One clause: "a && b || c && d" (and binds tighter than or)
static void ct_add_clause(ct_step& step, s32 id, const std::string& text, bool& valid)
{
	ct_clause clause;

	for (const std::string& alt_text : fmt::split(text, {"||"}))
	{
		ct_alt alt;

		for (const std::string& part : fmt::split(alt_text, {"&&"}))
		{
			ct_condition cond{};

			if (!ct_parse_condition(part, cond))
			{
				sceNpTrophy.error("Custom trophy rules: trophy %d: cannot understand condition '%s'", id, part);
				valid = false;
				return;
			}

			alt.emplace_back(std::move(cond));
		}

		if (!alt.empty())
		{
			clause.emplace_back(std::move(alt));
		}
	}

	if (!clause.empty())
	{
		step.clauses.emplace_back(std::move(clause));
	}
}

// Reads "when" (one step), "steps" (a sequence) and "reset" from a rule entry.
// A bare string or list is taken as "when". Returns false if the rule is unusable.
static bool ct_parse_rule(const YAML::Node& value, ct_rule& rule)
{
	bool valid = true;

	const auto add_step = [&](const YAML::Node& node, ct_step& step, std::string& text)
	{
		const auto add = [&](const std::string& clause)
		{
			ct_add_clause(step, rule.id, clause, valid);

			if (!text.empty())
			{
				text += " && ";
			}

			text += clause.find("||") != umax ? "(" + clause + ")" : clause;
		};

		if (node.IsScalar())
		{
			add(node.Scalar());
		}
		else if (node.IsSequence())
		{
			for (const auto& item : node)
			{
				if (valid && item.IsScalar())
				{
					add(item.Scalar());
				}
			}
		}
	};

	const auto add_whole_step = [&](const YAML::Node& node)
	{
		ct_step step;
		std::string text;
		add_step(node, step, text);

		if (!step.clauses.empty())
		{
			if (!rule.steps.empty())
			{
				rule.text += " THEN ";
			}

			rule.text += text;
			rule.steps.emplace_back(std::move(step));
		}
	};

	if (value.IsMap())
	{
		if (const YAML::Node when = value["when"]; when)
		{
			add_whole_step(when);
		}

		if (const YAML::Node steps = value["steps"]; steps && steps.IsSequence())
		{
			for (const auto& item : steps)
			{
				add_whole_step(item);
			}
		}

		if (const YAML::Node reset = value["reset"]; reset)
		{
			std::string text;
			add_step(reset, rule.reset, text);

			if (!text.empty())
			{
				rule.text += " (RESET IF " + text + ")";
			}
		}
	}
	else
	{
		add_whole_step(value);
	}

	if (!valid || rule.steps.empty())
	{
		return false;
	}

	// "value >= 50" on its own can be shown as progress
	if (rule.steps.size() == 1 && rule.steps[0].clauses.size() == 1 && rule.steps[0].clauses[0].size() == 1 && rule.steps[0].clauses[0][0].size() == 1)
	{
		const ct_condition& cond = rule.steps[0].clauses[0][0][0];

		if ((cond.cmp == ct_ge || cond.cmp == ct_gt) && !cond.hold_us && cond.rhs.size() == 1 && cond.rhs[0].type == ct_push && cond.rhs[0].value >= 1)
		{
			rule.has_progress = true;
			rule.progress_target = cond.rhs[0].value;
		}
	}

	return true;
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

			if (!ct_parse_rule(entry.second, rule) || set.is_removed(rule.id))
			{
				continue;
			}

			sceNpTrophy.notice("Custom trophy rules: trophy %d when %s", rule.id, rule.text);
			set.rules.emplace_back(std::move(rule));
		}
	}

	if (const YAML::Node node = root["custom"]; node && node.IsSequence())
	{
		for (const auto& entry : node)
		{
			if (!entry.IsMap())
			{
				continue;
			}

			const YAML::Node& value = entry;
			ct_custom_trophy trophy{};

			if (const YAML::Node n = value["name"]; n && n.IsScalar())
			{
				trophy.name = n.Scalar();
			}

			if (const YAML::Node n = value["detail"]; n && n.IsScalar())
			{
				trophy.detail = n.Scalar();
			}

			if (const YAML::Node n = value["icon"]; n && n.IsScalar())
			{
				trophy.icon = n.Scalar();
			}

			if (const YAML::Node n = value["grade"]; n && n.IsScalar() && !n.Scalar().empty())
			{
				const char grade = n.Scalar()[0];
				trophy.grade = (grade == 'G' || grade == 'g') ? 'G' : (grade == 'S' || grade == 's') ? 'S' : 'B';
			}

			if (const YAML::Node n = value["hidden"]; n && n.IsScalar())
			{
				trophy.hidden = n.Scalar() == "true" || n.Scalar() == "yes";
			}

			if (const YAML::Node n = value["deleted"]; n && n.IsScalar())
			{
				trophy.deleted = n.Scalar() == "true" || n.Scalar() == "yes";
			}

			if (trophy.deleted && trophy.name.empty())
			{
				trophy.name = "Removed trophy";
			}

			if (trophy.name.empty())
			{
				sceNpTrophy.error("Custom trophy rules: a custom trophy in '%s' has no name, skipping", used_path);
				continue;
			}

			if (trophy.deleted)
			{
				// No rule: it only holds its place
			}
			else if (rules_allowed && !ct_parse_rule(value, trophy.rule))
			{
				// Keep the trophy, it just cannot be earned until the rule is fixed
				trophy.rule = {};
			}

			set.custom.emplace_back(std::move(trophy));
		}
	}

	sceNpTrophy.success("Custom trophy rules: loaded '%s' (%d rules, %d removed, %d custom trophies)", used_path, set.rules.size(), set.removed.size(), set.custom.size());
}

static std::string ct_xml_escape(const std::string& text)
{
	std::string out;

	for (const char c : text)
	{
		switch (c)
		{
		case '&': out += "&amp;"; break;
		case '<': out += "&lt;"; break;
		case '>': out += "&gt;"; break;
		case '"': out += "&quot;"; break;
		default: out += c; break;
		}
	}

	return out;
}

// Adds the pack's custom trophies to the freshly installed trophy data: the definitions go
// into the .SFM files, the icons next to the game's own, and their rules get their ids.
// Returns the total number of trophies now defined (0 if the config could not be read).
// The game's TROPHY.TRP itself is never modified.
static u32 ct_merge_custom(ct_rules_set& set, const std::string& trp_name)
{
	static constexpr std::string_view s_marker = "<!--custom-trophies-->";
	static constexpr std::string_view s_end = "</trophyconf>";

	const std::string home = "/dev_hdd0/home/" + Emu.GetUsr();
	const std::string dir = vfs::get(home + "/trophy/" + trp_name + "/");
	const std::string icon_dir = vfs::get(home + "/trophy_rules/" + trp_name + "/");

	const auto read_clean = [&](const std::string& path, std::string& text)
	{
		fs::file file{path};

		if (!file)
		{
			return false;
		}

		text = file.to_string();

		// Drop anything a previous merge left behind
		if (const usz mark = text.find(s_marker); mark != umax)
		{
			if (const usz end = text.find(s_end, mark); end != umax)
			{
				text.erase(mark, end - mark);
			}
		}

		return true;
	};

	std::string conf;

	if (dir.empty() || !read_clean(dir + "TROPCONF.SFM", conf))
	{
		return 0;
	}

	// Official trophies: highest id + 1
	u32 count = 0;

	for (usz pos = 0; (pos = conf.find("<trophy id=\"", pos)) != umax; pos += 12)
	{
		const long id = std::strtol(conf.c_str() + pos + 12, nullptr, 10);

		if (id >= 0 && id < 0x10000)
		{
			count = std::max<u32>(count, static_cast<u32>(id) + 1);
		}
	}

	if (count == 0 || set.custom.empty())
	{
		return count;
	}

	if (count + set.custom.size() > 128)
	{
		sceNpTrophy.error("Custom trophy rules: too many custom trophies (a set can hold 128 in total)");
		return count;
	}

	if (set.original_count < 0)
	{
		set.original_count = static_cast<s32>(count);
	}

	std::string blocks{s_marker};
	blocks += '\n';

	for (usz i = 0; i < set.custom.size(); i++)
	{
		ct_custom_trophy& trophy = set.custom[i];
		const u32 id = count + static_cast<u32>(i);

		fmt::append(blocks, " <trophy id=\"%03u\" hidden=\"%s\" ttype=\"%s\" pid=\"-1\">\n  <name>%s</name>\n  <detail>%s</detail>\n </trophy>\n",
			id, trophy.hidden ? "yes" : "no", std::string(1, trophy.grade), ct_xml_escape(trophy.name), ct_xml_escape(trophy.detail));

		// Icon: the pack's own if it has one, otherwise the game's icon
		const std::string dst = dir + fmt::format("TROP%03u.PNG", id);
		const bool safe_name = !trophy.icon.empty() && trophy.icon.find('/') == umax && trophy.icon.find('\\') == umax && trophy.icon.find("..") == umax;

		if (!safe_name || icon_dir.empty() || !fs::copy_file(icon_dir + trophy.icon, dst, true))
		{
			if (!fs::copy_file(dir + "ICON0.PNG", dst, true))
			{
				sceNpTrophy.warning("Custom trophy rules: no icon for custom trophy %d", id);
			}
		}

		if (trophy.deleted)
		{
			set.removed.insert(id);
		}
		else if (!trophy.rule.steps.empty())
		{
			ct_rule rule = trophy.rule;
			rule.id = static_cast<s32>(id);
			sceNpTrophy.notice("Custom trophy rules: custom trophy %d '%s' when %s", id, trophy.name, rule.text);
			set.rules.emplace_back(std::move(rule));
		}
		else
		{
			sceNpTrophy.warning("Custom trophy rules: custom trophy %d '%s' has no usable rule, it cannot be earned", id, trophy.name);
		}
	}

	// The same definitions go into every config file that was installed
	std::vector<std::string> names{"TROPCONF.SFM", "TROP.SFM"};

	for (u32 i = 0; i <= 18; i++)
	{
		names.emplace_back(fmt::format("TROP_%02d.SFM", i));
	}

	bool conf_written = false;

	for (const std::string& name : names)
	{
		std::string text;

		if (!read_clean(dir + name, text))
		{
			continue;
		}

		const usz end = text.rfind(s_end);

		if (end == umax)
		{
			continue;
		}

		text.insert(end, blocks);

		if (fs::file out{dir + name, fs::rewrite}; out && out.write(text.data(), text.size()) == text.size())
		{
			conf_written |= name == "TROPCONF.SFM";
		}
		else
		{
			sceNpTrophy.error("Custom trophy rules: failed to update %s", name);
		}
	}

	if (!conf_written)
	{
		// Without the definitions the rules must not run either
		std::erase_if(set.rules, [&](const ct_rule& rule) { return rule.id >= static_cast<s32>(count); });
		return count;
	}

	sceNpTrophy.success("Custom trophy rules: added %d custom trophies after the game's %d", set.custom.size(), count);
	return count + static_cast<u32>(set.custom.size());
}

// Tells the trophy list (which reads the trophy folder directly) which ids to leave out
static void ct_write_hidden(const ct_rules_set& set, const std::string& trp_name)
{
	const std::string dir = vfs::get("/dev_hdd0/home/" + Emu.GetUsr() + "/trophy/" + trp_name + "/");

	if (dir.empty() || set.removed.empty())
	{
		return;
	}

	std::string text;

	for (const u32 id : set.removed)
	{
		fmt::append(text, "%u\n", id);
	}

	if (fs::file out{dir + "TROPHIDE.TXT", fs::rewrite})
	{
		out.write(text.data(), text.size());
	}
}

// For the memory search's watch list: the current value of an expression such as
// "u16[[[0x1f6a00]+0xf4]+0x4]". False if it cannot be parsed or read right now.
bool sceNpTrophy_eval_expression(std::string_view text, f64& result)
{
	std::vector<ct_op> ops;

	ct_parser parser{};
	parser.s = text;
	parser.out = &ops;

	if (!parser.cmp_expr() || !parser.at_end())
	{
		return false;
	}

	for (const ct_op& op : ops)
	{
		// These need state or a trophy context, which a one-off read does not have
		if (op.type == ct_delta || op.type == ct_prev || op.type == ct_count || op.type == ct_trophy)
		{
			return false;
		}
	}

	return ct_eval(ops, ct_eval_ctx{}, result);
}

// Calls func(rule id, op) for every operation of every rule, always in the same order
template <typename F>
static void ct_for_each_op(ct_rules_set& set, F&& func)
{
	const auto walk_step = [&](s32 id, ct_step& step)
	{
		for (ct_clause& clause : step.clauses)
		{
			for (ct_alt& alt : clause)
			{
				for (ct_condition& cond : alt)
				{
					for (ct_op& op : cond.lhs)
					{
						func(id, op);
					}

					for (ct_op& op : cond.rhs)
					{
						func(id, op);
					}
				}
			}
		}
	};

	for (ct_rule& rule : set.rules)
	{
		for (ct_step& step : rule.steps)
		{
			walk_step(rule.id, step);
		}

		walk_step(rule.id, rule.reset);
	}
}

static std::string ct_counts_path(const std::string& trp_name)
{
	return vfs::get("/dev_hdd0/home/" + Emu.GetUsr() + "/trophy_rules/" + trp_name + "_counts.txt");
}

// count() totals survive restarts. One line per counter: "<trophy id> <nth count() of that rule> <total>"
static void ct_load_counts(ct_rules_set& set, const std::string& trp_name)
{
	set.fast = false;
	set.has_counts = false;

	std::map<std::pair<s32, u32>, f64> saved;

	if (fs::file file{ct_counts_path(trp_name)})
	{
		for (const std::string& line : fmt::split(file.to_string(), {"\n", "\r"}))
		{
			int id = 0;
			unsigned index = 0;
			double total = 0;

			if (std::sscanf(line.c_str(), "%d %u %lf", &id, &index, &total) == 3 && total >= 0)
			{
				saved[{id, index}] = total;
			}
		}
	}

	s32 last_id = -1;
	u32 index = 0;
	f64 sum = 0;

	ct_for_each_op(set, [&](s32 id, ct_op& op)
	{
		if (id != last_id)
		{
			last_id = id;
			index = 0;
		}

		if (op.type == ct_button)
		{
			set.fast = true;
		}
		else if (op.type == ct_count)
		{
			set.has_counts = true;

			if (const auto found = saved.find({id, index}); found != saved.end())
			{
				op.state = found->second;
			}

			sum += op.state;
			index++;
		}
	});

	set.saved_counts = sum;
}

static void ct_save_counts(ct_rules_set& set, const std::string& trp_name)
{
	std::string text;
	s32 last_id = -1;
	u32 index = 0;
	f64 sum = 0;

	ct_for_each_op(set, [&](s32 id, ct_op& op)
	{
		if (id != last_id)
		{
			last_id = id;
			index = 0;
		}

		if (op.type == ct_count)
		{
			fmt::append(text, "%d %u %d\n", id, index, static_cast<s64>(op.state));
			sum += op.state;
			index++;
		}
	});

	if (sum == set.saved_counts)
	{
		return;
	}

	const std::string path = ct_counts_path(trp_name);

	if (fs::create_path(fs::get_parent_dir(path)))
	{
		if (fs::file out{path, fs::rewrite})
		{
			out.write(text.data(), text.size());
			set.saved_counts = sum;
		}
	}
}

// Must be called with sce_np_trophy_manager::mtx held
static ct_rules_set& ct_get_rules(const std::string& trp_name, bool reload = false)
{
	ct_rules_set& set = g_fxo->get<custom_trophy_state>().sets[trp_name];

	if (reload || !set.loaded)
	{
		ct_load(set, trp_name);

		// Also done when a savestate skipped sceNpTrophyRegisterContext; merging twice is harmless
		set.defined_trophies = ct_merge_custom(set, trp_name);
		ct_write_hidden(set, trp_name);
		ct_load_counts(set, trp_name);
	}

	return set;
}

// Trophy tracer: records which game code asked for a trophy, once per trophy and session.
// The addresses are where to start looking (in the decrypted EBOOT) for the values that the
// game checks, i.e. the raw material for writing custom trophy rules.
// Written to the log and to home/<user>/trophy_rules/<NPWRxxxxx_00>_trace.txt.
// Must be called with sce_np_trophy_manager::mtx held, on the PPU thread that made the request.
static void ct_trace_request(ppu_thread& ppu, const trophy_context_t* ctxt, s32 trophyId, bool already_unlocked)
{
	if (!g_fxo->get<custom_trophy_state>().traced[ctxt->trp_name].insert(trophyId).second)
	{
		return;
	}

	// The game called the import stub from here
	std::string chain = fmt::format("0x%x", static_cast<u32>(ppu.lr) - 4);

	// Walk the PPC64 stack: [sp] is the caller's frame, its saved LR is at +16
	u32 sp = static_cast<u32>(ppu.gpr[1]);

	for (u32 depth = 0; depth < 8; depth++)
	{
		be_t<u64> back{};

		if (!vm::try_access(sp, &back, sizeof(back), false))
		{
			break;
		}

		const u32 next = static_cast<u32>(back.value());

		if (next <= sp)
		{
			break;
		}

		be_t<u64> saved_lr{};

		if (!vm::try_access(next + 16, &saved_lr, sizeof(saved_lr), false))
		{
			break;
		}

		const u32 ret = static_cast<u32>(saved_lr.value());

		if (ret < 0x10000)
		{
			break;
		}

		fmt::append(chain, " <- 0x%x", ret - 4);
		sp = next;
	}

	const std::string line = fmt::format("trophy %d%s: requested from %s (r4=0x%x r5=0x%x r6=0x%x)", trophyId, already_unlocked ? " (already unlocked)" : "", chain, static_cast<u32>(ppu.gpr[4]), static_cast<u32>(ppu.gpr[5]), static_cast<u32>(ppu.gpr[6]));

	sceNpTrophy.success("Trophy trace: %s", line);

	const std::string dir = vfs::get("/dev_hdd0/home/" + Emu.GetUsr() + "/trophy_rules/");

	if (dir.empty() || !fs::create_path(dir))
	{
		return;
	}

	const std::string path = dir + ctxt->trp_name + "_trace.txt";
	const bool is_new = !fs::is_file(path);

	if (fs::file file{path, fs::write + fs::create + fs::append})
	{
		if (is_new)
		{
			file.write(fmt::format("# Trophy trace for %s\n# game: %s (%s)\n# Each line: the game code that asked for the trophy, innermost call first.\n", ctxt->trp_name, Emu.GetExecutableHash(), Emu.GetTitleID()));
		}

		file.write(line + "\n");
	}
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
	static atomic_t<u64> s_interval = 250'000;

	const u64 now = get_system_time();

	if (now < s_next_check)
	{
		return;
	}

	s_next_check = now + s_interval.load();

	// Button rules need to see every frame, everything else is fine at 4x a second
	bool any_fast = false;

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

		any_fast |= rules.fast;

		const s32 count = static_cast<s32>(ctxt->tropusr->GetTrophiesCount());

		if (!rules.platinum_checked)
		{
			// Removing trophies may have completed a platinum that was already earned otherwise
			rules.platinum_checked = true;

			// First check for this set: say so if a rule can never fire
			for (const ct_rule& rule : rules.rules)
			{
				if (rule.id >= count)
				{
					sceNpTrophy.error("Custom trophy rules: trophy %d has a rule but the trophy data only has %d trophies. TROPHY.TRP and TROPUSR.DAT must both include it.", rule.id, count);
				}
				else if (ctxt->tropusr->GetTrophyUnlockState(rule.id))
				{
					sceNpTrophy.notice("Custom trophy rules: trophy %d is already unlocked", rule.id);
				}
				else
				{
					sceNpTrophy.notice("Custom trophy rules: trophy %d is locked, watching (%s)", rule.id, rule.text);
				}
			}

			for (const u32 id : rules.removed)
			{
				if (id < static_cast<u32>(count))
				{
					np_trophy_unlock_impl(ppu, ctxt, rules, static_cast<s32>(id), false);
				}
			}
		}

		ct_eval_ctx eval_ctx{};
		eval_ctx.tropusr = ctxt->tropusr.get();
		// A long gap means the game was paused or stalled; that time does not count
		eval_ctx.dt_us = rules.last_check ? std::min<u64>(now - rules.last_check, 1'000'000) : 0;
		rules.last_check = now;

		for (ct_rule& rule : rules.rules)
		{
			if (rule.id < 0 || rule.id >= count)
			{
				continue;
			}

			if (ctxt->tropusr->GetTrophyUnlockState(rule.id) || ctxt->tropusr->GetTrophyGrade(rule.id) == SCE_NP_TROPHY_GRADE_PLATINUM)
			{
				continue;
			}

			const bool earned = ct_test(rule, eval_ctx);

			if (rule.has_progress)
			{
				const ct_condition& cond = rule.steps[0].clauses[0][0][0];
				const s64 shown = cond.readable && cond.last >= 0 && cond.last < 9.0e15 ? static_cast<s64>(cond.last) : -1;

				if (shown != rule.progress_shown)
				{
					rule.progress_shown = shown;
					rules.progress_dirty = true;
				}
			}

			if (!earned)
			{
				continue;
			}

			sceNpTrophy.success("Custom trophy rules: trophy %d earned (%s)", rule.id, rule.text);
			np_trophy_unlock_impl(ppu, ctxt, rules, rule.id, true);
			rules.progress_dirty = true;
		}

		// Progress of simple counting rules, for the trophy list ("37 / 50")
		if (rules.progress_dirty && now >= rules.next_progress_write)
		{
			rules.progress_dirty = false;
			rules.next_progress_write = now + 2'000'000;

			std::string text;

			for (const ct_rule& rule : rules.rules)
			{
				if (rule.has_progress && rule.progress_shown >= 0 && rule.id >= 0 && rule.id < count && !ctxt->tropusr->GetTrophyUnlockState(rule.id))
				{
					fmt::append(text, "%d %d %d\n", rule.id, rule.progress_shown, static_cast<s64>(rule.progress_target));
				}
			}

			const std::string path = vfs::get("/dev_hdd0/home/" + Emu.GetUsr() + "/trophy/" + ctxt->trp_name + "/TROPPROG.TXT");

			if (fs::file out{path, fs::rewrite})
			{
				out.write(text.data(), text.size());
			}
		}

		if (rules.has_counts && now >= rules.next_counts_write)
		{
			rules.next_counts_write = now + 5'000'000;
			ct_save_counts(rules, ctxt->trp_name);
		}
	}

	s_interval = any_fast ? 12'000 : 250'000;
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

	// (Re)load the optional custom trophy rules that belong to this trophy set, and add its
	// custom trophies to the definitions that were just installed
	const ct_rules_set custom_rules = ct_get_rules(ctxt->trp_name, true);
	const u32 defined_trophies = custom_rules.defined_trophies;

	ensure(tropusr->Load(trophyUsrPath, trophyConfPath).success);

	if (defined_trophies != 0 && tropusr->GetTrophiesCount() != defined_trophies)
	{
		// The trophy set grew or shrank (custom trophies were added or removed): rebuild the
		// progress file for the new set, keeping everything that was already earned
		sceNpTrophy.warning("sceNpTrophyRegisterContext(): TROPUSR.DAT has %d trophies but %d are defined, resizing", tropusr->GetTrophiesCount(), defined_trophies);

		if (!tropusr->SyncWithConfig(trophyUsrPath, trophyConfPath))
		{
			sceNpTrophy.error("sceNpTrophyRegisterContext(): Failed to resize TROPUSR.DAT");
		}
	}

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

	ct_trace_request(ppu, ctxt, trophyId, !!ctxt->tropusr->GetTrophyUnlockState(trophyId));

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
