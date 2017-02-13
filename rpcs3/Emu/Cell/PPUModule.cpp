﻿#include "stdafx.h"
#include "Utilities/Config.h"
#include "Utilities/AutoPause.h"
#include "Crypto/sha1.h"
#include "Crypto/unself.h"
#include "Loader/ELF.h"
#include "Emu/System.h"
#include "Emu/IdManager.h"

#include "Emu/Cell/PPUOpcodes.h"
#include "Emu/Cell/PPUModule.h"
#include "Emu/Cell/PPUAnalyser.h"

#include "Emu/Cell/lv2/sys_prx.h"

#include <map>
#include <set>
#include <algorithm>

namespace vm { using namespace ps3; }

LOG_CHANNEL(cellAdec);
LOG_CHANNEL(cellAtrac);
LOG_CHANNEL(cellAtracMulti);
LOG_CHANNEL(cellAudio);
LOG_CHANNEL(cellAvconfExt);
LOG_CHANNEL(cellBGDL);
LOG_CHANNEL(cellCamera);
LOG_CHANNEL(cellCelp8Enc);
LOG_CHANNEL(cellCelpEnc);
LOG_CHANNEL(cellDaisy);
LOG_CHANNEL(cellDmux);
LOG_CHANNEL(cellFiber);
LOG_CHANNEL(cellFont);
LOG_CHANNEL(cellFontFT);
LOG_CHANNEL(cell_FreeType2);
LOG_CHANNEL(cellFs);
LOG_CHANNEL(cellGame);
LOG_CHANNEL(cellGameExec);
LOG_CHANNEL(cellGcmSys);
LOG_CHANNEL(cellGem);
LOG_CHANNEL(cellGifDec);
LOG_CHANNEL(cellHttp);
LOG_CHANNEL(cellHttpUtil);
LOG_CHANNEL(cellImeJp);
LOG_CHANNEL(cellJpgDec);
LOG_CHANNEL(cellJpgEnc);
LOG_CHANNEL(cellKey2char);
LOG_CHANNEL(cellL10n);
LOG_CHANNEL(cellMic);
LOG_CHANNEL(cellMusic);
LOG_CHANNEL(cellMusicDecode);
LOG_CHANNEL(cellMusicExport);
LOG_CHANNEL(cellNetCtl);
LOG_CHANNEL(cellOskDialog);
LOG_CHANNEL(cellOvis);
LOG_CHANNEL(cellPamf);
LOG_CHANNEL(cellPhotoDecode);
LOG_CHANNEL(cellPhotoExport);
LOG_CHANNEL(cellPhotoImportUtil);
LOG_CHANNEL(cellPngDec);
LOG_CHANNEL(cellPngEnc);
LOG_CHANNEL(cellPrint);
LOG_CHANNEL(cellRec);
LOG_CHANNEL(cellRemotePlay);
LOG_CHANNEL(cellResc);
LOG_CHANNEL(cellRtc);
LOG_CHANNEL(cellRudp);
LOG_CHANNEL(cellSail);
LOG_CHANNEL(cellSailRec);
LOG_CHANNEL(cellSaveData);
LOG_CHANNEL(cellScreenshot);
LOG_CHANNEL(cellSearch);
LOG_CHANNEL(cellSheap);
LOG_CHANNEL(cellSpudll);
LOG_CHANNEL(cellSpurs);
LOG_CHANNEL(cellSpursJq);
LOG_CHANNEL(cellSsl);
LOG_CHANNEL(cellSubdisplay);
LOG_CHANNEL(cellSync);
LOG_CHANNEL(cellSync2);
LOG_CHANNEL(cellSysconf);
LOG_CHANNEL(cellSysmodule);
LOG_CHANNEL(cellSysutil);
LOG_CHANNEL(cellSysutilAp);
LOG_CHANNEL(cellSysutilAvc);
LOG_CHANNEL(cellSysutilAvc2);
LOG_CHANNEL(cellSysutilMisc);
LOG_CHANNEL(cellUsbd);
LOG_CHANNEL(cellUsbPspcm);
LOG_CHANNEL(cellUserInfo);
LOG_CHANNEL(cellVdec);
LOG_CHANNEL(cellVideoExport);
LOG_CHANNEL(cellVideoUpload);
LOG_CHANNEL(cellVoice);
LOG_CHANNEL(cellVpost);
LOG_CHANNEL(libmixer);
LOG_CHANNEL(libsnd3);
LOG_CHANNEL(libsynth2);
LOG_CHANNEL(sceNp);
LOG_CHANNEL(sceNp2);
LOG_CHANNEL(sceNpClans);
LOG_CHANNEL(sceNpCommerce2);
LOG_CHANNEL(sceNpSns);
LOG_CHANNEL(sceNpTrophy);
LOG_CHANNEL(sceNpTus);
LOG_CHANNEL(sceNpUtil);
LOG_CHANNEL(sys_io);
LOG_CHANNEL(sys_libc);
LOG_CHANNEL(sys_lv2dbg);
LOG_CHANNEL(libnet);
LOG_CHANNEL(sysPrxForUser);

cfg::bool_entry g_cfg_hook_ppu_funcs(cfg::root.core, "Hook static functions");
cfg::bool_entry g_cfg_load_liblv2(cfg::root.core, "Load liblv2.sprx only");
cfg::bool_entry g_cfg_load_libreq(cfg::root.core, "Load required libraries", true);

cfg::set_entry g_cfg_load_libs(cfg::root.core, "Load libraries");

extern std::string ppu_get_function_name(const std::string& module, u32 fnid);
extern std::string ppu_get_variable_name(const std::string& module, u32 vnid);
extern void ppu_register_range(u32 addr, u32 size);
extern void ppu_register_function_at(u32 addr, u32 size, ppu_function_t ptr);
extern void ppu_initialize(const ppu_module& info);
extern void ppu_initialize();

extern void sys_initialize_tls(ppu_thread&, u64, u32, u32, u32);

extern u32 g_ps3_sdk_version;

extern u32 ppu_generate_id(const char* name)
{
	// Symbol name suffix
	const auto suffix = "\x67\x59\x65\x99\x04\x25\x04\x90\x56\x64\x27\x49\x94\x89\x74\x1A";

	sha1_context ctx;
	u8 output[20];

	// Compute SHA-1 hash
	sha1_starts(&ctx);
	sha1_update(&ctx, reinterpret_cast<const u8*>(name), std::strlen(name));
	sha1_update(&ctx, reinterpret_cast<const u8*>(suffix), std::strlen(suffix));
	sha1_finish(&ctx, output);

	return reinterpret_cast<le_t<u32>&>(output[0]);
}

ppu_static_module::ppu_static_module(const char* name)
	: name(name)
{
	ppu_module_manager::register_module(this);
}

std::unordered_map<std::string, ppu_static_module*>& ppu_module_manager::access()
{
	static std::unordered_map<std::string, ppu_static_module*> map;

	return map;
}

void ppu_module_manager::register_module(ppu_static_module* module)
{
	access().emplace(module->name, module);
}

ppu_static_function& ppu_module_manager::access_static_function(const char* module, u32 fnid)
{
	return access().at(module)->functions[fnid];
}

ppu_static_variable& ppu_module_manager::access_static_variable(const char* module, u32 vnid)
{
	return access().at(module)->variables[vnid];
}

const ppu_static_module* ppu_module_manager::get_module(const std::string& name)
{
	const auto& map = access();
	const auto found = map.find(name);
	return found != map.end() ? found->second : nullptr;
}

// Global linkage information
struct ppu_linkage_info
{
	struct module
	{
		struct info
		{
			u32 export_addr = 0;
			std::set<u32> imports;
		};

		// FNID -> (export; [imports...])
		std::map<u32, info> functions;
		std::map<u32, info> variables;
	};

	// Module map
	std::unordered_map<std::string, module> modules;
};

// Initialize static modules.
static void ppu_initialize_modules()
{
	const std::initializer_list<const ppu_static_module*> registered
	{
		&ppu_module_manager::cellAdec,
		&ppu_module_manager::cellAtrac,
		&ppu_module_manager::cellAtracMulti,
		&ppu_module_manager::cellAudio,
		&ppu_module_manager::cellAvconfExt,
		&ppu_module_manager::cellBGDL,
		&ppu_module_manager::cellCamera,
		&ppu_module_manager::cellCelp8Enc,
		&ppu_module_manager::cellCelpEnc,
		&ppu_module_manager::cellDaisy,
		&ppu_module_manager::cellDmux,
		&ppu_module_manager::cellFiber,
		&ppu_module_manager::cellFont,
		&ppu_module_manager::cellFontFT,
		&ppu_module_manager::cell_FreeType2,
		&ppu_module_manager::cellFs,
		&ppu_module_manager::cellGame,
		&ppu_module_manager::cellGameExec,
		&ppu_module_manager::cellGcmSys,
		&ppu_module_manager::cellGem,
		&ppu_module_manager::cellGifDec,
		&ppu_module_manager::cellHttp,
		&ppu_module_manager::cellHttps,
		&ppu_module_manager::cellHttpUtil,
		&ppu_module_manager::cellImeJp,
		&ppu_module_manager::cellJpgDec,
		&ppu_module_manager::cellJpgEnc,
		&ppu_module_manager::cellKey2char,
		&ppu_module_manager::cellL10n,
		&ppu_module_manager::cellMic,
		&ppu_module_manager::cellMusic,
		&ppu_module_manager::cellMusicDecode,
		&ppu_module_manager::cellMusicExport,
		&ppu_module_manager::cellNetCtl,
		&ppu_module_manager::cellOskDialog,
		&ppu_module_manager::cellOvis,
		&ppu_module_manager::cellPamf,
		&ppu_module_manager::cellPhotoDecode,
		&ppu_module_manager::cellPhotoExport,
		&ppu_module_manager::cellPhotoImportUtil,
		&ppu_module_manager::cellPngDec,
		&ppu_module_manager::cellPngEnc,
		&ppu_module_manager::cellPrint,
		&ppu_module_manager::cellRec,
		&ppu_module_manager::cellRemotePlay,
		&ppu_module_manager::cellResc,
		&ppu_module_manager::cellRtc,
		&ppu_module_manager::cellRudp,
		&ppu_module_manager::cellSail,
		&ppu_module_manager::cellSailRec,
		&ppu_module_manager::cellSaveData,
		&ppu_module_manager::cellMinisSaveData,
		&ppu_module_manager::cellScreenShot,
		&ppu_module_manager::cellSearch,
		&ppu_module_manager::cellSheap,
		&ppu_module_manager::cellSpudll,
		&ppu_module_manager::cellSpurs,
		&ppu_module_manager::cellSpursJq,
		&ppu_module_manager::cellSsl,
		&ppu_module_manager::cellSubdisplay,
		&ppu_module_manager::cellSync,
		&ppu_module_manager::cellSync2,
		&ppu_module_manager::cellSysconf,
		&ppu_module_manager::cellSysmodule,
		&ppu_module_manager::cellSysutil,
		&ppu_module_manager::cellSysutilAp,
		&ppu_module_manager::cellSysutilAvc,
		&ppu_module_manager::cellSysutilAvc2,
		&ppu_module_manager::cellSysutilMisc,
		&ppu_module_manager::cellUsbd,
		&ppu_module_manager::cellUsbPspcm,
		&ppu_module_manager::cellUserInfo,
		&ppu_module_manager::cellVdec,
		&ppu_module_manager::cellVideoExport,
		&ppu_module_manager::cellVideoUpload,
		&ppu_module_manager::cellVoice,
		&ppu_module_manager::cellVpost,
		&ppu_module_manager::libmixer,
		&ppu_module_manager::libsnd3,
		&ppu_module_manager::libsynth2,
		&ppu_module_manager::sceNp,
		&ppu_module_manager::sceNp2,
		&ppu_module_manager::sceNpClans,
		&ppu_module_manager::sceNpCommerce2,
		&ppu_module_manager::sceNpSns,
		&ppu_module_manager::sceNpTrophy,
		&ppu_module_manager::sceNpTus,
		&ppu_module_manager::sceNpUtil,
		&ppu_module_manager::sys_io,
		&ppu_module_manager::libnet,
		&ppu_module_manager::sysPrxForUser,
		&ppu_module_manager::sys_libc,
		&ppu_module_manager::sys_lv2dbg,
	};

	// "Use" all the modules for correct linkage
	for (auto& module : registered)
	{
		LOG_TRACE(LOADER, "Registered static module: %s", module->name);

		for (auto& function : module->functions)
		{
			LOG_TRACE(LOADER, "** 0x%08X: %s", function.first, function.second.name);
		}

		for (auto& variable : module->variables)
		{
			LOG_TRACE(LOADER, "** &0x%08X: %s (size=0x%x, align=0x%x)", variable.first, variable.second.name, variable.second.size, variable.second.align);
			variable.second.var->set(0);
		}
	}

	// Initialize double-purpose fake OPD array for HLE functions
	const auto& hle_funcs = ppu_function_manager::get();

	// Allocate memory for the array (must be called after fixed allocations)
	ppu_function_manager::addr = vm::alloc(::size32(hle_funcs) * 8, vm::main);

	// Initialize as PPU executable code
	ppu_register_range(ppu_function_manager::addr, ::size32(hle_funcs) * 8);

	// Fill the array (visible data: self address and function index)
	for (u32 addr = ppu_function_manager::addr, index = 0; index < hle_funcs.size(); addr += 8, index++)
	{
		// Function address = current address, RTOC = BLR instruction for the interpreter
		vm::ps3::write32(addr + 0, addr);
		vm::ps3::write32(addr + 4, ppu_instructions::BLR());

		// Register the HLE function directly
		ppu_register_function_at(addr + 0, 4, hle_funcs[index]);
	}

	// Set memory protection to read-only
	vm::page_protect(ppu_function_manager::addr, ::align(::size32(hle_funcs) * 8, 0x1000), 0, 0, vm::page_writable);
}

// Link variable
static void ppu_patch_variable_refs(u32 vref, u32 vaddr)
{
	struct vref_t
	{
		be_t<u32> type;
		be_t<u32> addr;
		be_t<u32> unk0;
	};

	for (auto ref = vm::ptr<vref_t>::make(vref); ref->type; ref++)
	{
		if (ref->unk0) LOG_ERROR(LOADER, "**** VREF(%u): Unknown values (0x%x, 0x%x)", ref->type, ref->addr, ref->unk0);

		// OPs are probably similar to relocations
		switch (u32 type = ref->type)
		{
		case 0x1:
		{
			const u32 value = vm::_ref<u32>(ref->addr) = vaddr;
			LOG_WARNING(LOADER, "**** VREF(1): 0x%x <- 0x%x", ref->addr, value);
			break;
		}

		case 0x4:
		case 0x6:
		default: LOG_ERROR(LOADER, "**** VREF(%u): Unknown/Illegal type (0x%x, 0x%x)", ref->type, ref->addr, ref->unk0);
		}
	}
}

// Export or import module struct
struct ppu_prx_module_info
{
	u8 size;
	u8 unk0;
	be_t<u16> version;
	be_t<u16> attributes;
	be_t<u16> num_func;
	be_t<u16> num_var;
	be_t<u16> num_tlsvar;
	u8 info_hash;
	u8 info_tlshash;
	u8 unk1[2];
	vm::bcptr<char> name;
	vm::bcptr<u32> nids; // Imported FNIDs, Exported NIDs
	vm::bptr<u32> addrs;
	vm::bcptr<u32> vnids; // Imported VNIDs
	vm::bcptr<u32> vstubs;
	be_t<u32> unk4;
	be_t<u32> unk5;
};

// Load and register exports; return special exports found (nameless module)
static auto ppu_load_exports(const std::shared_ptr<ppu_linkage_info>& link, u32 exports_start, u32 exports_end)
{
	std::unordered_map<u32, u32> result;

	for (u32 addr = exports_start; addr < exports_end;)
	{
		const auto& lib = vm::_ref<const ppu_prx_module_info>(addr);

		if (!lib.name)
		{
			// Set special exports
			for (u32 i = 0, end = lib.num_func + lib.num_var; i < end; i++)
			{
				const u32 nid = lib.nids[i];
				const u32 addr = lib.addrs[i];

				if (i < lib.num_func)
				{
					LOG_NOTICE(LOADER, "** Special: [%s] at 0x%x", ppu_get_function_name({}, nid), addr);
				}
				else
				{
					LOG_NOTICE(LOADER, "** Special: &[%s] at 0x%x", ppu_get_variable_name({}, nid), addr);
				}

				result.emplace(nid, addr);
			}

			addr += lib.size ? lib.size : sizeof(ppu_prx_module_info);
			continue;
		}

		const std::string module_name(lib.name.get_ptr());

		LOG_NOTICE(LOADER, "** Exported module '%s' (0x%x, 0x%x, 0x%x, 0x%x)", module_name, lib.vnids, lib.vstubs, lib.unk4, lib.unk5);

		if (lib.num_tlsvar)
		{
			LOG_FATAL(LOADER, "Unexpected num_tlsvar (%u)!", lib.num_tlsvar);
		}

		// Static module
		const auto _sm = ppu_module_manager::get_module(module_name);

		const auto fnids = +lib.nids;
		const auto faddrs = +lib.addrs;

		// Get functions
		for (u32 i = 0, end = lib.num_func; i < end; i++)
		{
			const u32 fnid = fnids[i];
			const u32 faddr = faddrs[i];
			LOG_NOTICE(LOADER, "**** %s export: [%s] at 0x%x", module_name, ppu_get_function_name(module_name, fnid), faddr);

			// Function linkage info
			auto& flink = link->modules[module_name].functions[fnid];

			if (flink.export_addr)
			{
				LOG_FATAL(LOADER, "Already linked function '%s' in module '%s'", ppu_get_function_name(module_name, fnid), module_name);
			}
			else
			{
				// Static function
				const auto _sf = _sm && _sm->functions.count(fnid) ? &_sm->functions.at(fnid) : nullptr;

				if (_sf && (_sf->flags & MFF_FORCED_HLE))
				{
					// Inject a branch to the HLE implementation
					const u32 _entry = vm::read32(faddr);
					const u32 target = ppu_function_manager::addr + 8 * _sf->index;

					if ((target <= _entry && _entry - target <= 0x2000000) || (target > _entry && target - _entry < 0x2000000))
					{
						// Use relative branch
						vm::write32(_entry, ppu_instructions::B(target - _entry));
					}
					else if (target < 0x2000000)
					{
						// Use absolute branch if possible
						vm::write32(_entry, ppu_instructions::B(target, true));
					}
					else
					{
						LOG_FATAL(LOADER, "Failed to patch function at 0x%x (0x%x)", _entry, target);
					}
				}
				else
				{
					// Set exported function
					flink.export_addr = faddr;

					// Fix imports
					for (const u32 addr : flink.imports)
					{
						vm::write32(addr, faddr);
						//LOG_WARNING(LOADER, "Exported function '%s' in module '%s'", ppu_get_function_name(module_name, fnid), module_name);
					}
				}
			}
		}

		const auto vnids = lib.nids + lib.num_func;
		const auto vaddrs = lib.addrs + lib.num_func;

		// Get variables
		for (u32 i = 0, end = lib.num_var; i < end; i++)
		{
			const u32 vnid = vnids[i];
			const u32 vaddr = vaddrs[i];
			LOG_NOTICE(LOADER, "**** %s export: &[%s] at 0x%x", module_name, ppu_get_variable_name(module_name, vnid), vaddr);

			// Variable linkage info
			auto& vlink = link->modules[module_name].variables[vnid];

			if (vlink.export_addr)
			{
				LOG_FATAL(LOADER, "Already linked variable '%s' in module '%s'", ppu_get_variable_name(module_name, vnid), module_name);
			}
			else
			{
				// Set exported variable
				vlink.export_addr = vaddr;

				// Fix imports
				for (const auto vref : vlink.imports)
				{
					ppu_patch_variable_refs(vref, vaddr);
					//LOG_WARNING(LOADER, "Exported variable '%s' in module '%s'", ppu_get_variable_name(module_name, vnid), module_name);
				}
			}
		}

		addr += lib.size ? lib.size : sizeof(ppu_prx_module_info);
	}

	return result;
}

static void ppu_load_imports(const std::shared_ptr<ppu_linkage_info>& link, u32 imports_start, u32 imports_end)
{
	for (u32 addr = imports_start; addr < imports_end;)
	{
		const auto& lib = vm::_ref<const ppu_prx_module_info>(addr);

		const std::string module_name(lib.name.get_ptr());

		LOG_NOTICE(LOADER, "** Imported module '%s' (0x%x, 0x%x)", module_name, lib.unk4, lib.unk5);

		if (lib.num_tlsvar)
		{
			LOG_FATAL(LOADER, "Unexpected num_tlsvar (%u)!", lib.num_tlsvar);
		}

		// Static module
		const auto _sm = ppu_module_manager::get_module(module_name);

		const auto fnids = +lib.nids;
		const auto faddrs = +lib.addrs;

		for (u32 i = 0, end = lib.num_func; i < end; i++)
		{
			const u32 fnid = fnids[i];
			const u32 fstub = faddrs[i];
			const u32 faddr = (faddrs + i).addr();
			LOG_NOTICE(LOADER, "**** %s import: [%s] -> 0x%x", module_name, ppu_get_function_name(module_name, fnid), fstub);

			// Function linkage info
			auto& flink = link->modules[module_name].functions[fnid];

			// Add new import
			flink.imports.emplace(faddr);

			// Link if available
			if (flink.export_addr)
			{
				vm::write32(faddr, flink.export_addr);
			}

			//LOG_WARNING(LOADER, "Imported function '%s' in module '%s' (0x%x)", ppu_get_function_name(module_name, fnid), module_name, faddr);
		}

		const auto vnids = +lib.vnids;
		const auto vstubs = +lib.vstubs;

		for (u32 i = 0, end = lib.num_var; i < end; i++)
		{
			const u32 vnid = vnids[i];
			const u32 vref = vstubs[i];
			LOG_NOTICE(LOADER, "**** %s import: &[%s] (ref=*0x%x)", module_name, ppu_get_variable_name(module_name, vnid), vref);

			// Variable linkage info
			auto& vlink = link->modules[module_name].variables[vnid];

			// Add new import
			vlink.imports.emplace(vref);

			// Link if available
			if (vlink.export_addr)
			{
				ppu_patch_variable_refs(vref, vlink.export_addr);
			}

			//LOG_WARNING(LOADER, "Imported variable '%s' in module '%s' (0x%x)", ppu_get_variable_name(module_name, vnid), module_name, vlink.first);
		}

		addr += lib.size ? lib.size : sizeof(ppu_prx_module_info);
	}
}

std::shared_ptr<lv2_prx> ppu_load_prx(const ppu_prx_object& elf, const std::string& name)
{
	std::vector<std::pair<u32, u32>> segments;
	std::vector<std::pair<u32, u32>> sections;

	for (const auto& prog : elf.progs)
	{
		LOG_NOTICE(LOADER, "** Segment: p_type=0x%x, p_vaddr=0x%llx, p_filesz=0x%llx, p_memsz=0x%llx, flags=0x%x", prog.p_type, prog.p_vaddr, prog.p_filesz, prog.p_memsz, prog.p_flags);

		switch (const u32 p_type = prog.p_type)
		{
		case 0x1: // LOAD
		{
			if (prog.p_memsz)
			{
				const u32 mem_size = ::narrow<u32>(prog.p_memsz, "p_memsz" HERE);
				const u32 file_size = ::narrow<u32>(prog.p_filesz, "p_filesz" HERE);
				const u32 init_addr = ::narrow<u32>(prog.p_vaddr, "p_vaddr" HERE);

				// Alloc segment memory
				const u32 addr = vm::alloc(mem_size, vm::main);

				if (!addr)
				{
					fmt::throw_exception("vm::alloc() failed (size=0x%x)", mem_size);
				}

				// Copy segment data
				std::memcpy(vm::base(addr), prog.bin.data(), file_size);
				LOG_WARNING(LOADER, "**** Loaded to 0x%x (size=0x%x)", addr, mem_size);

				// Initialize executable code if necessary
				if (prog.p_flags & 0x1)
				{
					ppu_register_range(addr, mem_size);
				}

				segments.emplace_back(std::make_pair(addr, mem_size));
			}

			break;
		}

		case 0x700000a4: break; // Relocations

		default: LOG_ERROR(LOADER, "Unknown segment type! 0x%08x", p_type);
		}
	}

	for (const auto& s : elf.shdrs)
	{
		LOG_NOTICE(LOADER, "** Section: sh_type=0x%x, addr=0x%llx, size=0x%llx, flags=0x%x", s.sh_type, s.sh_addr, s.sh_size, s.sh_flags);

		const u32 addr = vm::cast(s.sh_addr);
		const u32 size = vm::cast(s.sh_size);

		if (s.sh_type == 1 && addr && size) // TODO: some sections with addr=0 are valid
		{
			for (auto i = 0; i < segments.size(); i++)
			{
				const u32 saddr = static_cast<u32>(elf.progs[i].p_vaddr);
				if (addr >= saddr && addr < saddr + elf.progs[i].p_memsz)
				{
					// "Relocate" section
					sections.emplace_back(std::make_pair(addr - saddr + segments[i].first, size));
					break;
				}
			}
		}
	}

	// Do relocations
	for (auto& prog : elf.progs)
	{
		switch (const u32 p_type = prog.p_type)
		{
		case 0x700000a4:
		{
			// Relocation information of the SCE_PPURELA segment
			struct ppu_prx_relocation_info
			{
				be_t<u64> offset;
				be_t<u16> unk0;
				u8 index_value;
				u8 index_addr;
				be_t<u32> type;
				vm::bptr<void, u64> ptr;
			};

			for (uint i = 0; i < prog.p_filesz; i += sizeof(ppu_prx_relocation_info))
			{
				const auto& rel = reinterpret_cast<const ppu_prx_relocation_info&>(prog.bin[i]);

				const u32 raddr = vm::cast(segments.at(rel.index_addr).first + rel.offset, HERE);
				const u64 rdata = segments.at(rel.index_value).first + rel.ptr.addr();

				switch (const u32 type = rel.type)
				{
				case 1:
				{
					const u32 value = vm::_ref<u32>(raddr) = static_cast<u32>(rdata);
					LOG_TRACE(LOADER, "**** RELOCATION(1): 0x%x <- 0x%08x (0x%llx)", raddr, value, rdata);
					break;
				}

				case 4:
				{
					const u16 value = vm::_ref<u16>(raddr) = static_cast<u16>(rdata);
					LOG_TRACE(LOADER, "**** RELOCATION(4): 0x%x <- 0x%04x (0x%llx)", raddr, value, rdata);
					break;
				}

				case 5:
				{
					const u16 value = vm::_ref<u16>(raddr) = static_cast<u16>(rdata >> 16);
					LOG_TRACE(LOADER, "**** RELOCATION(5): 0x%x <- 0x%04x (0x%llx)", raddr, value, rdata);
					break;
				}

				case 6:
				{
					const u16 value = vm::_ref<u16>(raddr) = static_cast<u16>(rdata >> 16) + (rdata & 0x8000 ? 1 : 0);
					LOG_TRACE(LOADER, "**** RELOCATION(6): 0x%x <- 0x%04x (0x%llx)", raddr, value, rdata);
					break;
				}

				case 10:
				{
					const u32 value = vm::_ref<ppu_bf_t<be_t<u32>, 6, 24>>(raddr) = static_cast<u32>(rdata - raddr) >> 2;
					LOG_WARNING(LOADER, "**** RELOCATION(10): 0x%x <- 0x%06x (0x%llx)", raddr, value, rdata);
					break;
				}

				case 44:
				{
					const u64 value = vm::_ref<u64>(raddr) = rdata - raddr;
					LOG_TRACE(LOADER, "**** RELOCATION(44): 0x%x <- 0x%016llx (0x%llx)", raddr, value, rdata);
					break;
				}

				case 57:
				{
					const u16 value = vm::_ref<ppu_bf_t<be_t<u16>, 0, 14>>(raddr) = static_cast<u16>(rdata) >> 2;
					LOG_WARNING(LOADER, "**** RELOCATION(57): 0x%x <- 0x%04x (0x%llx)", raddr, value, rdata);
					break;
				}

				default: LOG_ERROR(LOADER, "**** RELOCATION(%u): Illegal/Unknown type! (addr=0x%x; 0x%llx)", type, raddr, rdata);
				}
			}

			break;
		}
		}
	}

	// Access linkage information object
	const auto link = fxm::get_always<ppu_linkage_info>();

	// Create new PRX object
	auto prx = idm::make_ptr<lv2_obj, lv2_prx>();

	if (!elf.progs.empty() && elf.progs[0].p_paddr)
	{
		struct ppu_prx_library_info
		{
			be_t<u16> attributes;
			be_t<u16> version;
			char name[28];
			be_t<u32> toc;
			be_t<u32> exports_start;
			be_t<u32> exports_end;
			be_t<u32> imports_start;
			be_t<u32> imports_end;
		};

		// Access library information (TODO)
		const auto& lib_info = vm::cptr<ppu_prx_library_info>(vm::cast(segments[0].first + elf.progs[0].p_paddr - elf.progs[0].p_offset, HERE));
		const auto& lib_name = std::string(lib_info->name);

		LOG_WARNING(LOADER, "Library %s (rtoc=0x%x):", lib_name, lib_info->toc);

		prx->specials = ppu_load_exports(link, lib_info->exports_start, lib_info->exports_end);

		ppu_load_imports(link, lib_info->imports_start, lib_info->imports_end);

		prx->funcs = ppu_analyse(segments, sections, lib_info->toc);
	}
	else
	{
		LOG_FATAL(LOADER, "Library %s: PRX library info not found");
	}

	prx->start.set(prx->specials[0xbc9a0086]);
	prx->stop.set(prx->specials[0xab779874]);
	prx->exit.set(prx->specials[0x3ab9a95e]);
	prx->name = name;
	return prx;
}

void ppu_load_exec(const ppu_exec_object& elf)
{
	if (g_cfg_hook_ppu_funcs)
	{
		LOG_TODO(LOADER, "'Hook static functions' option deactivated");
	}

	// Access linkage information object
	const auto link = fxm::get_always<ppu_linkage_info>();

	// Segment info
	std::vector<std::pair<u32, u32>> segments;

	// Section info (optional)
	std::vector<std::pair<u32, u32>> sections;

	// TLS information
	u32 tls_vaddr = 0;
	u32 tls_fsize = 0;
	u32 tls_vsize = 0;

	// Process information
	u32 sdk_version = 0x360001;
	s32 primary_prio = 0x50;
	u32 primary_stacksize = 0x100000;
	u32 malloc_pagesize = 0x100000;

	// Allocate memory at fixed positions
	for (const auto& prog : elf.progs)
	{
		LOG_NOTICE(LOADER, "** Segment: p_type=0x%x, p_vaddr=0x%llx, p_filesz=0x%llx, p_memsz=0x%llx, flags=0x%x", prog.p_type, prog.p_vaddr, prog.p_filesz, prog.p_memsz, prog.p_flags);

		const u32 addr = vm::cast(prog.p_vaddr, HERE);
		const u32 size = ::narrow<u32>(prog.p_memsz, "p_memsz" HERE);

		if (prog.p_type == 0x1 /* LOAD */ && prog.p_memsz)
		{
			if (prog.bin.size() > size || prog.bin.size() != prog.p_filesz)
				fmt::throw_exception("Invalid binary size (0x%llx, memsz=0x%x)", prog.bin.size(), size);

			if (!vm::falloc(addr, size, vm::main))
				fmt::throw_exception("vm::falloc() failed (addr=0x%x, memsz=0x%x)", addr, size);

			// Copy segment data
			std::memcpy(vm::base(addr), prog.bin.data(), prog.bin.size());

			// Initialize executable code if necessary
			if (prog.p_flags & 0x1)
			{
				ppu_register_range(addr, size);
			}

			segments.emplace_back(std::make_pair(addr, size));
		}
	}

	// Load section list, used by the analyser
	for (const auto& s : elf.shdrs)
	{
		LOG_NOTICE(LOADER, "** Section: sh_type=0x%x, addr=0x%llx, size=0x%llx, flags=0x%x", s.sh_type, s.sh_addr, s.sh_size, s.sh_flags);

		const u32 addr = vm::cast(s.sh_addr);
		const u32 size = vm::cast(s.sh_size);

		if (s.sh_type == 1 && addr && size)
		{
			sections.emplace_back(std::make_pair(addr, size));
		}
	}

	// Initialize HLE modules
	ppu_initialize_modules();

	// Load other programs
	for (auto& prog : elf.progs)
	{
		switch (const u32 p_type = prog.p_type)
		{
		case 0x00000001: break; // LOAD (already loaded)

		case 0x00000007: // TLS
		{
			tls_vaddr = vm::cast(prog.p_vaddr, HERE);
			tls_fsize = ::narrow<u32>(prog.p_filesz, "p_filesz" HERE);
			tls_vsize = ::narrow<u32>(prog.p_memsz, "p_memsz" HERE);
			break;
		}

		case 0x60000001: // LOOS+1
		{
			if (prog.p_filesz)
			{
				struct process_param_t
				{
					be_t<u32> size;
					be_t<u32> magic;
					be_t<u32> version;
					be_t<u32> sdk_version;
					be_t<s32> primary_prio;
					be_t<u32> primary_stacksize;
					be_t<u32> malloc_pagesize;
					be_t<u32> ppc_seg;
					//be_t<u32> crash_dump_param_addr;
				};

				const auto& info = vm::ps3::_ref<process_param_t>(vm::cast(prog.p_vaddr, HERE));

				if (info.size < sizeof(process_param_t))
				{
					LOG_WARNING(LOADER, "Bad process_param size! [0x%x : 0x%x]", info.size, SIZE_32(process_param_t));
				}

				if (info.magic != 0x13bcc5f6)
				{
					LOG_ERROR(LOADER, "Bad process_param magic! [0x%x]", info.magic);
				}
				else
				{
					sdk_version = info.sdk_version;
					primary_prio = info.primary_prio;
					primary_stacksize = info.primary_stacksize;
					malloc_pagesize = info.malloc_pagesize;

					LOG_NOTICE(LOADER, "*** sdk version: 0x%x", info.sdk_version);
					LOG_NOTICE(LOADER, "*** primary prio: %d", info.primary_prio);
					LOG_NOTICE(LOADER, "*** primary stacksize: 0x%x", info.primary_stacksize);
					LOG_NOTICE(LOADER, "*** malloc pagesize: 0x%x", info.malloc_pagesize);
					LOG_NOTICE(LOADER, "*** ppc seg: 0x%x", info.ppc_seg);
					//LOG_NOTICE(LOADER, "*** crash dump param addr: 0x%x", info.crash_dump_param_addr);
				}
			}
			break;
		}

		case 0x60000002: // LOOS+2
		{
			if (prog.p_filesz)
			{
				struct ppu_proc_prx_param_t
				{
					be_t<u32> size;
					be_t<u32> magic;
					be_t<u32> version;
					be_t<u32> unk0;
					be_t<u32> libent_start;
					be_t<u32> libent_end;
					be_t<u32> libstub_start;
					be_t<u32> libstub_end;
					be_t<u16> ver;
					be_t<u16> unk1;
					be_t<u32> unk2;
				};

				const auto& proc_prx_param = vm::_ref<const ppu_proc_prx_param_t>(vm::cast(prog.p_vaddr, HERE));

				LOG_NOTICE(LOADER, "* libent_start = *0x%x", proc_prx_param.libent_start);
				LOG_NOTICE(LOADER, "* libstub_start = *0x%x", proc_prx_param.libstub_start);
				LOG_NOTICE(LOADER, "* unk0 = 0x%x", proc_prx_param.unk0);
				LOG_NOTICE(LOADER, "* unk2 = 0x%x", proc_prx_param.unk2);

				if (proc_prx_param.magic != 0x1b434cec)
				{
					fmt::throw_exception("Bad magic! (0x%x)", proc_prx_param.magic);
				}

				ppu_load_exports(link, proc_prx_param.libent_start, proc_prx_param.libent_end);
				ppu_load_imports(link, proc_prx_param.libstub_start, proc_prx_param.libstub_end);
			}
			break;
		}
		default:
		{
			LOG_ERROR(LOADER, "Unknown phdr type (0x%08x)", p_type);
		}
		}
	}

	// Initialize process
	std::vector<std::shared_ptr<lv2_prx>> loaded_modules;

	// Get LLE module list
	std::set<std::string> load_libs;

	if (!!g_cfg_load_liblv2 == !!g_cfg_load_libreq)
	{
		// Load required set of modules
		load_libs = g_cfg_load_libs.get_set();
	}

	if (g_cfg_load_liblv2 && !g_cfg_load_libreq)
	{
		// Load only liblv2.sprx
		load_libs.emplace("liblv2.sprx");
	}
	else if (g_cfg_load_libreq)
	{
		// Load recommended set of modules: Module name -> SPRX
		const std::unordered_multimap<std::string, std::string> sprx_map
		{
			{ "cellAdec", "libadec.sprx" }, // cellSpurs|cell_libac3dec|cellAtrac3dec|cellAtracXdec|cellCelpDec|cellDTSdec|cellM2AACdec|cellM2BCdec|cellM4AacDec|cellMP3dec|cellTRHDdec|cellWMAdec|cellDTSLBRdec|cellDDPdec|cellM4AacDec2ch|cellDTSHDdec|cellMPL1dec|cellMP3Sdec|cellM4AacDec2chmod|cellCelp8Dec|cellWMAPROdec|cellWMALSLdec|cellDTSHDCOREdec|cellAtrac3multidec
			{ "cellAdec", "libsre.sprx" },
			{ "cellAdec", "libac3dec.sprx" },
			{ "cellAdec", "libat3dec.sprx" },
			{ "cellAdec", "libat3multidec.sprx" },
			{ "cellAdec", "libatxdec.sprx" },
			{ "cellAdec", "libcelp8dec.sprx" },
			{ "cellAdec", "libcelpdec.sprx" },
			{ "cellAdec", "libddpdec.sprx" },
			{ "cellAdec", "libm2bcdec.sprx" },
			{ "cellAdec", "libm4aacdec.sprx" },
			{ "cellAdec", "libm4aacdec2ch.sprx" },
			{ "cellAdec", "libmp3dec.sprx" },
			{ "cellAdec", "libmpl1dec.sprx" },
			{ "cellAdec", "libwmadec.sprx" },
			{ "cellAtrac", "libatrac3plus.sprx" },
			{ "cellAtrac", "cellAdec" },
			{ "cellAtracMulti", "libatrac3multi.sprx" },
			{ "cellAtracMulti", "cellAdec" },
			{ "cellCelp8Enc", "libcelp8enc.sprx" },
			{ "cellCelp8Enc", "libsre.sprx" },
			{ "cellCelpEnc", "libcelpenc.sprx" },
			{ "cellCelpEnc", "libsre.sprx" },
			{ "cellDmux", "libdmux.sprx" },
			{ "cellDmux", "libdmuxpamf.sprx" },
			{ "cellDmux", "libsre.sprx" },
			{ "cellFiber", "libfiber.sprx" },
			{ "cellFont", "libfont.sprx" },
			{ "cellFontFT", "libfontFT.sprx" },
			{ "cellFontFT", "libfreetype.sprx" },
			{ "cellGifDec", "libgifdec.sprx" },
			{ "cellGifDec", "libsre.sprx" },
			{ "cellJpgDec", "libjpgdec.sprx" },
			{ "cellJpgDec", "libsre.sprx" },
			{ "cellJpgEnc", "libjpgenc.sprx" },
			{ "cellJpgEnc", "libsre.sprx" },
			{ "cellKey2char", "libkey2char.sprx" },
			{ "cellL10n", "libl10n.sprx" },
			{ "cellM4hdEnc", "libm4hdenc.sprx" },
			{ "cellM4hdEnc", "libsre.sprx" },
			{ "cellPamf", "libpamf.sprx" },
			{ "cellPngDec", "libpngdec.sprx" },
			{ "cellPngDec", "libsre.sprx" },
			{ "cellPngEnc", "libpngenc.sprx" },
			{ "cellPngEnc", "libsre.sprx" },
			{ "cellResc", "libresc.sprx" },
			{ "cellRtc", "librtc.sprx" },
			{ "cellSsl", "libssl.sprx" },
			{ "cellSsl", "librtc.sprx" },
			{ "cellSail", "libsail.sprx" },
			{ "cellSail", "libsre.sprx" },
			{ "cellSail", "libmp4.sprx" },
			{ "cellSail", "libpamf.sprx" },
			{ "cellSail", "libdmux.sprx" },
			{ "cellSail", "libdmuxpamf.sprx" },
			{ "cellSail", "libapostsrc_mini.sprx" },
			{ "cellSail", "libsail_avi.sprx" },
			{ "cellSail", "libvpost.sprx" },
			{ "cellSail", "cellAdec" },
			{ "cellSpursJq", "libspurs_jq.sprx" },
			{ "cellSpursJq", "libsre.sprx" },
			{ "cellSync", "libsre.sprx" },
			{ "cellSheap", "libsre.sprx" },
			{ "cellOvis", "libsre.sprx" },
			{ "cellSpurs", "libsre.sprx" },
			{ "cellDaisy", "libsre.sprx" },
			{ "cellSpudll", "libsre.sprx" },
			{ "cellSync2", "libsync2.sprx" },
			{ "cellSync2", "libsre.sprx" },
			{ "cellVpost", "libvpost.sprx" },
			{ "cellVpost", "libsre.sprx" },
		};

		for (const auto& pair : link->modules)
		{
			for (auto range = sprx_map.equal_range(pair.first); range.first != range.second;)
			{
				// Dependencies (workaround for cellAdec)
				auto range2 = sprx_map.equal_range(range.first->second);

				if (range2.first != range2.second)
				{
					range = range2;
				}
				else
				{
					load_libs.emplace(range.first->second);
					range.first++;
				}
			}
		}
	}
	
	if (!load_libs.empty())
	{
		const std::string lle_dir = vfs::get("/dev_flash/sys/external/");

		if (!fs::is_dir(lle_dir))
		{
			LOG_ERROR(GENERAL, "/dev_flash/sys/external/ directory does not exist!"
				"\nYou should install the PS3 Firmware (Menu: Tools -> Install Firmware)."
				"\nVisit https://rpcs3.net/ for Quickstart Guide and more information.");
		}

		for (const auto& name : load_libs)
		{
			const ppu_prx_object obj = decrypt_self(fs::file(lle_dir + name));

			if (obj == elf_error::ok)
			{
				LOG_WARNING(LOADER, "Loading library: %s", name);

				auto prx = ppu_load_prx(obj, name);

				if (prx->funcs.empty())
				{
					LOG_FATAL(LOADER, "Module %s has no functions!", name);
				}
				else
				{
					// TODO: fix arguments
					ppu_validate(lle_dir + name, prx->funcs, prx->funcs[0].addr);
				}

				loaded_modules.emplace_back(std::move(prx));
			}
			else
			{
				fmt::throw_exception("Failed to load /dev_flash/sys/external/%s: %s", name, obj.get_error());
			}
		}
	}

	// Check unlinked functions and variables
	for (auto& module : link->modules)
	{
		const auto _sm = ppu_module_manager::get_module(module.first);

		if (!_sm)
		{
			LOG_ERROR(LOADER, "Unknown module '%s'", module.first);
		}
		else
		{
			// Allocate HLE variables (TODO)
			for (auto& var : _sm->variables)
			{
				var.second.var->set(vm::alloc(var.second.size, vm::main, std::max<u32>(var.second.align, 4096)));
				LOG_WARNING(LOADER, "Allocated variable '%s' in module '%s' at *0x%x", var.second.name, module.first, var.second.var->addr());
			}

			// Initialize HLE variables (TODO)
			for (auto& var : _sm->variables)
			{
				var.second.init();
			}
		}

		for (auto& entry : module.second.functions)
		{
			const u32 fnid = entry.first;
			const u32 faddr = entry.second.export_addr;

			if (faddr == 0)
			{
				const std::string fname = ppu_get_function_name(module.first, fnid);

				// Link HLE implementation if available
				if (const auto _sf = _sm && _sm->functions.count(fnid) ? &_sm->functions.at(fnid) : nullptr)
				{
					LOG_NOTICE(LOADER, "Linking HLE function '%s' in module '%s' (index %u)", fname, module.first, _sf->index);

					for (const u32 import : entry.second.imports)
					{
						LOG_TRACE(LOADER, "** Linked at *0x%x (0x%x)", import, vm::read32(import));
						vm::write32(import, ppu_function_manager::addr + 8 * _sf->index);
					}
				}
				else
				{
					LOG_ERROR(LOADER, "Unknown function '%s' in module '%s'", fname, module.first);

					for (const u32 import : entry.second.imports)
					{
						LOG_WARNING(LOADER, "** Not linked at *0x%x (0x%x)", import, vm::read32(import));
						vm::write32(import, ppu_function_manager::addr);
					}
				}
			}
		}

		for (auto& entry : module.second.variables)
		{
			const u32 vnid = entry.first;
			const u32 vaddr = entry.second.export_addr;

			if (vaddr == 0)
			{
				const std::string vname = ppu_get_variable_name(module.first, vnid);

				// Link HLE variable if available
				if (const auto _sv = _sm && _sm->variables.count(vnid) ? &_sm->variables.at(vnid) : nullptr)
				{
					LOG_NOTICE(LOADER, "Linking HLE variable '%s' in module '%s' (*0x%x):", vname, module.first, _sv->var->addr());

					for (const u32 ref : entry.second.imports)
					{
						ppu_patch_variable_refs(ref, _sv->var->addr());
						LOG_NOTICE(LOADER, "** Linked at ref=*0x%x", ref);
					}
				}
				else
				{
					LOG_ERROR(LOADER, "Unknown variable '%s' in module '%s'", vname, module.first);

					for (const u32 ref : entry.second.imports)
					{
						LOG_WARNING(LOADER, "** Not linked at ref=*0x%x", ref);
					}
				}
			}
			else
			{
				// Retro-link LLE variable (TODO: HLE must not be allocated/initialized in this case)
				if (const auto _sv = _sm && _sm->variables.count(vnid) ? &_sm->variables.at(vnid) : nullptr)
				{
					_sv->var->set(vaddr);
					LOG_NOTICE(LOADER, "Linked LLE variable '%s' in module '%s' -> 0x%x", ppu_get_variable_name(module.first, vnid), module.first, vaddr);
				}
			}
		}
	}

	{
		// Analyse executable
		std::vector<ppu_function> main_funcs = ppu_analyse(segments, sections, 0);

		ppu_validate(vfs::get(Emu.GetPath()), main_funcs, 0);

		// Share function list
		fxm::make<std::vector<ppu_function>>(std::move(main_funcs));
	}

	// Set SDK version
	g_ps3_sdk_version = sdk_version;

	// Initialize process arguments
	std::initializer_list<std::string> args = { Emu.GetPath()/*, "-emu"s*/ };

	auto argv = vm::ptr<u64>::make(vm::alloc(SIZE_32(u64) * ::size32(args), vm::main));
	auto envp = vm::ptr<u64>::make(vm::alloc(::align(SIZE_32(u64), 0x10), vm::main));
	*envp = 0;

	for (const auto& arg : args)
	{
		const u32 arg_size = ::align(::size32(arg) + 1, 0x10);
		const u32 arg_addr = vm::alloc(arg_size, vm::main);

		std::memcpy(vm::base(arg_addr), arg.data(), arg_size);

		*argv++ = arg_addr;
	}

	argv -= args.size();

	// Initialize main thread
	auto ppu = idm::make_ptr<ppu_thread>("main_thread", primary_prio, primary_stacksize);

	ppu->cmd_push({ppu_cmd::initialize, 0});

	// TODO: adjust for liblv2 loading option
	if (!g_cfg_load_liblv2)
	{
		// Set TLS args, call sys_initialize_tls
		ppu->cmd_list
		({
			{ ppu_cmd::set_args, 4 }, u64{ppu->id}, u64{tls_vaddr}, u64{tls_fsize}, u64{tls_vsize},
			{ ppu_cmd::hle_call, FIND_FUNC(sys_initialize_tls) },
		});
	}

	// Run start functions
	for (const auto& prx : loaded_modules)
	{
		if (!prx->start)
		{
			continue;
		}

		// Reset arguments, run module entry point function
		ppu->cmd_list
		({
			{ ppu_cmd::set_args, 2 }, u64{0}, u64{0},
			{ ppu_cmd::lle_call, prx->start.addr() },
		});
	}

	// Set command line arguments, run entry function
	ppu->cmd_list
	({
		{ ppu_cmd::set_args, 8 }, u64{args.size()}, u64{argv.addr()}, u64{envp.addr()}, u64{0}, u64{ppu->id}, u64{tls_vaddr}, u64{tls_fsize}, u64{tls_vsize},
		{ ppu_cmd::set_gpr, 11 }, u64{0xabadcafe},
		{ ppu_cmd::set_gpr, 12 }, u64{malloc_pagesize},
		{ ppu_cmd::lle_call, static_cast<u32>(elf.header.e_entry) },
	});

	// Set actual memory protection (experimental)
	for (const auto& prog : elf.progs)
	{
		const u32 addr = static_cast<u32>(prog.p_vaddr);
		const u32 size = static_cast<u32>(prog.p_memsz);

		if (prog.p_type == 0x1 /* LOAD */ && prog.p_memsz && (prog.p_flags & 0x2) == 0 /* W */)
		{
			// Set memory protection to read-only when necessary
			verify(HERE), vm::page_protect(addr, ::align(size, 0x1000), 0, 0, vm::page_writable);
		}
	}
}
