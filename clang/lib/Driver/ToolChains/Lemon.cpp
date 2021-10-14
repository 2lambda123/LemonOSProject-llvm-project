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
    getFilePaths().push_back(getDriver().SysRoot + "/system/lib");
}

void Lemon::addLibCxxIncludePaths(const llvm::opt::ArgList &DriverArgs,
                            llvm::opt::ArgStringList &CC1Args) const {
    addSystemInclude(DriverArgs, CC1Args, getDriver().SysRoot + "/system/include/c++/v1");
    addSystemInclude(DriverArgs, CC1Args, getDriver().Dir + "/../include/c++/v1");
    addSystemInclude(DriverArgs, CC1Args, getDriver().Dir + "/../include/" + getTripleString() + "/c++/v1");
}

void Lemon::AddCXXStdlibLibArgs(const llvm::opt::ArgList &Args,
                                  llvm::opt::ArgStringList &CmdArgs) const {
    CXXStdlibType Type = GetCXXStdlibType(Args);

    switch (Type) {
    case ToolChain::CST_Libcxx:
        CmdArgs.push_back("-lc++");
        break;
    }
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

    if (Args.hasArg(options::OPT_static)) {
        CmdArgs.push_back("-Bstatic");
    } else {
        if (Args.hasArg(options::OPT_rdynamic))
            CmdArgs.push_back("-export-dynamic");
        if (Args.hasArg(options::OPT_shared)) {
            CmdArgs.push_back("-shared");
        }
    }

    CmdArgs.push_back("-rpath=/system/lib");

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

    if(!Args.hasArg(options::OPT_nostdlib, options::OPT_nostartfiles)){
        if(!Args.hasArg(options::OPT_shared)){
            CmdArgs.push_back(Args.MakeArgString(ToolChain.GetFilePath("crt0.o")));
        }
        std::string crtbegin = ToolChain.getCompilerRT(Args, "crtbegin",
                                                       ToolChain::FT_Object);
        if (ToolChain.getVFS().exists(crtbegin)){
            CmdArgs.push_back(Args.MakeArgString(ToolChain.GetFilePath(crtbegin.c_str())));
        }
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

    AddLinkerInputs(ToolChain, Inputs, Args, CmdArgs, JA);

    if (!Args.hasArg(options::OPT_nostdlib, options::OPT_nodefaultlibs)) {
        AddRunTimeLibs(ToolChain, D, CmdArgs, Args);

        if (D.CCCIsCXX()) {
            if (ToolChain.ShouldLinkCXXStdlib(Args))
                ToolChain.AddCXXStdlibLibArgs(Args, CmdArgs);
        }

        if (Args.hasArg(options::OPT_pthread)) {
            CmdArgs.push_back("-lpthread");
        }

        CmdArgs.push_back("-lc");
    }

    if (!Args.hasArg(options::OPT_nostdlib, options::OPT_nostartfiles)) {
        std::string crtend = ToolChain.getCompilerRT(Args, "crtend",
                                                       ToolChain::FT_Object);
        if (ToolChain.getVFS().exists(crtend)){
            CmdArgs.push_back(Args.MakeArgString(ToolChain.GetFilePath(crtend.c_str())));
        }
    }

    const char *Exec = Args.MakeArgString(getToolChain().GetLinkerPath());
    C.addCommand(std::make_unique<Command>(JA, *this, ResponseFileSupport::AtFileCurCP(), Exec, CmdArgs, Inputs));
}
