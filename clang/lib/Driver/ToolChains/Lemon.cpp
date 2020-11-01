#include "Lemon.h"

#include "CommonArgs.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/DriverDiagnostic.h"
#include "clang/Driver/Options.h"
#include "clang/Driver/SanitizerArgs.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/VirtualFileSystem.h"

using namespace clang::driver;
using namespace clang::driver::tools;
using namespace clang::driver::toolchains;
using namespace clang;
using namespace llvm::opt;

Lemon::Lemon(const Driver &D, const llvm::Triple &Triple, const llvm::opt::ArgList &Args)
    : Generic_ELF(D, Triple, Args){
    
}

Tool *Lemon::buildAssembler() const {
    return new tools::lemon::Assembler(*this);
}

Tool *Lemon::buildLinker() const {
    return new tools::lemon::Linker(*this);
}

void lemon::Assembler::ConstructJob(Compilation &C, const JobAction &JA,
                        const InputInfo &Output, const InputInfoList &Inputs,
                        const llvm::opt::ArgList &Args,
                        const char *LinkingOutput) const {
    ArgStringList CmdArgs;
    const Driver &D = getToolChain().getDriver();
    
    CmdArgs.push_back("-o");
    CmdArgs.push_back(Output.getFilename());

    for (const auto &II : Inputs)
        CmdArgs.push_back(II.getFilename());

    Args.AddAllArgValues(CmdArgs, options::OPT_Wa_COMMA, options::OPT_Xassembler);
  
    const char *Exec = Args.MakeArgString(getToolChain().GetProgramPath("as"));
    C.addCommand(std::make_unique<Command>(
        JA, *this, ResponseFileSupport::AtFileCurCP(), Exec, CmdArgs, Inputs));
}

void lemon::Linker::ConstructJob(Compilation &C, const JobAction &JA,
                        const InputInfo &Output, const InputInfoList &Inputs,
                        const llvm::opt::ArgList &Args,
                        const char *LinkingOutput) const{
    const toolchains::Lemon &ToolChain = static_cast<const toolchains::Lemon&>(getToolChain());
    ArgStringList CmdArgs;
  
    const Driver &D = ToolChain.getDriver();
    const llvm::Triple::ArchType Arch = ToolChain.getArch();
    const bool IsPIE = !Args.hasArg(options::OPT_shared) && (Args.hasArg(options::OPT_pie) || ToolChain.isPIEDefault());
      
    // Silence warning for "clang -g foo.o -o foo"
    Args.ClaimAllArgs(options::OPT_g_Group);
    // and "clang -emit-llvm foo.o -o foo"
    Args.ClaimAllArgs(options::OPT_emit_llvm);
    // and for "clang -w foo.o -o foo". Other warning options are already
    // handled somewhere else.
    Args.ClaimAllArgs(options::OPT_w);
    
    if (!D.SysRoot.empty())
        CmdArgs.push_back(Args.MakeArgString("--sysroot=" + D.SysRoot));

    if (IsPIE)
        CmdArgs.push_back("-pie");
    
    if (Output.isFilename()) {
        CmdArgs.push_back("-o");
        CmdArgs.push_back(Output.getFilename());
    } else {
        assert(Output.isNothing() && "Invalid output.");
    }
  
    if(!Args.hasArg(options::OPT_nostdlib) && !Args.hasArg(options::OPT_nostartfiles)){
        if(!Args.hasArg(options::OPT_shared)){
            CmdArgs.push_back(Args.MakeArgString(ToolChain.GetFilePath("crt0.o")));
        }
        
        if(Args.hasArg(options::OPT_shared) || IsPIE){
            CmdArgs.push_back(Args.MakeArgString(ToolChain.GetFilePath("crtbeginS.o")));
        } else {
            CmdArgs.push_back(Args.MakeArgString(ToolChain.GetFilePath("crtbegin.o")));
        }
        CmdArgs.push_back(Args.MakeArgString(ToolChain.GetFilePath("crti.o")));
    }
    
    Args.AddAllArgs(CmdArgs, options::OPT_L);
    ToolChain.AddFilePathLibArgs(Args, CmdArgs);
    Args.AddAllArgs(CmdArgs, options::OPT_T_Group);
    Args.AddAllArgs(CmdArgs, options::OPT_e);
    Args.AddAllArgs(CmdArgs, options::OPT_s);
    Args.AddAllArgs(CmdArgs, options::OPT_t);
    Args.AddAllArgs(CmdArgs, options::OPT_Z_Flag);
    Args.AddAllArgs(CmdArgs, options::OPT_r);

    if (D.isUsingLTO()) {
        assert(!Inputs.empty() && "Must have at least one input.");
        addLTOOptions(ToolChain, Args, CmdArgs, Output, Inputs[0],
                    D.getLTOMode() == LTOK_Thin);
    }

    bool NeedsSanitizerDeps = addSanitizerRuntimes(ToolChain, Args, CmdArgs);
    bool NeedsXRayDeps = addXRayRuntime(ToolChain, Args, CmdArgs);
    AddLinkerInputs(ToolChain, Inputs, Args, CmdArgs, JA);
    
    if (!Args.hasArg(options::OPT_nostdlib, options::OPT_nodefaultlibs)) {
        if (D.CCCIsCXX()) {
            if (ToolChain.ShouldLinkCXXStdlib(Args))
                ToolChain.AddCXXStdlibLibArgs(Args, CmdArgs);
            CmdArgs.push_back("-lm");
        }
        
        if (NeedsSanitizerDeps)
            linkSanitizerRuntimeDeps(ToolChain, CmdArgs);
        if (NeedsXRayDeps)
            linkXRayRuntimeDeps(ToolChain, CmdArgs);
        
        if (Args.hasArg(options::OPT_pthread)) {
            CmdArgs.push_back("-lpthread");
        }
    }

    if (!Args.hasArg(options::OPT_nostdlib, options::OPT_nostartfiles)) {
        if (Args.hasArg(options::OPT_shared) || IsPIE)
        CmdArgs.push_back(Args.MakeArgString(ToolChain.GetFilePath("crtendS.o")));
        else
        CmdArgs.push_back(Args.MakeArgString(ToolChain.GetFilePath("crtend.o")));
        CmdArgs.push_back(Args.MakeArgString(ToolChain.GetFilePath("crtn.o")));
    }

    ToolChain.addProfileRTLibs(Args, CmdArgs);

    const char *Exec = Args.MakeArgString(getToolChain().GetLinkerPath());
    C.addCommand(std::make_unique<Command>(JA, *this, ResponseFileSupport::AtFileCurCP(), Exec, CmdArgs, Inputs));
}
