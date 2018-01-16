// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#define UNICODE
#include "common.h"

#include <cstring>
#include <string>
#include <vector>
#include <sstream>
#include <unordered_map>
#include <functional>
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <iomanip>

#include "platform.h"
#include "debugger.h"
#include "modules.h"
#include "breakpoints.h"
#include "varobj.h"
#include "frames.h"


using namespace std::placeholders;

typedef std::function<HRESULT(
    ICorDebugProcess *pProcess,
    const std::vector<std::string> &args,
    std::string &output)> CommandCallback;


static int ParseInt(const std::string &s, bool &ok)
{
    ok = false;
    try
    {
        int result = std::stoi(s);
        ok = true;
        return result;
    }
    catch(std::invalid_argument e)
    {
    }
    catch (std::out_of_range  e)
    {
    }
    return 0;
}

// Remove all --name value
static void StripArgs(std::vector<std::string> &args)
{
    auto it = args.begin();

    while (it != args.end())
    {
        if (it->find("--") == 0 && it + 1 != args.end())
            it = args.erase(args.erase(it));
        else
            ++it;
    }
}

static int GetIntArg(const std::vector<std::string> &args, const std::string name, int defaultValue)
{
    auto it = std::find(args.begin(), args.end(), name);

    if (it == args.end())
        return defaultValue;

    ++it;

    if (it == args.end())
        return defaultValue;

    bool ok;
    int val = ParseInt(*it, ok);
    return ok ? val : defaultValue;
}

static bool GetIndices(const std::vector<std::string> &args, int &index1, int &index2)
{
    if (args.size() < 2)
        return false;

    bool ok;
    int val1 = ParseInt(args.at(args.size() - 2), ok);
    if (!ok)
        return false;
    int val2 = ParseInt(args.at(args.size() - 1), ok);
    if (!ok)
        return false;
    index1 = val1;
    index2 = val2;
    return true;
}

bool ParseBreakpoint(const std::vector<std::string> &args_orig, std::string &filename, unsigned int &linenum)
{
    std::vector<std::string> args = args_orig;
    StripArgs(args);

    if (args.empty())
        return false;

    if (args.at(0) == "-f")
    {
        args.erase(args.begin());
        if (args.empty())
            return false;
    }

    std::size_t i = args.at(0).rfind(':');

    if (i == std::string::npos)
        return false;

    filename = args.at(0).substr(0, i);
    std::string slinenum = args.at(0).substr(i + 1);

    bool ok;
    linenum = ParseInt(slinenum, ok);
    return ok && linenum > 0;
}


static HRESULT PrintBreakpoint(const Breakpoint &b, std::string &output)
{
    HRESULT Status;

    std::stringstream ss;

    if (b.verified)
    {
        ss << "bkpt={number=\"" << b.id << "\",type=\"breakpoint\",disp=\"keep\",enabled=\"y\","
            "func=\"\",fullname=\"" << MIProtocol::EscapeMIValue(b.source.path) << "\",line=\"" << b.line << "\"}";
        Status = S_OK;
    }
    else
    {
        ss << "bkpt={number=\"" << b.id << "\",type=\"breakpoint\",disp=\"keep\",enabled=\"y\","
            "warning=\"No executable code of the debugger's target code type is associated with this line.\"}";
        Status = S_FALSE;
    }
    output = ss.str();
    return Status;
}

void MIProtocol::EmitBreakpointEvent(BreakpointEvent event)
{
    switch(event.reason)
    {
        case StopBreakpoint:
        {
            std::string output;
            PrintBreakpoint(event.breakpoint, output);
            MIProtocol::Printf("=breakpoint-modified,%s\n", output.c_str());
            return;
        }
        default:
            break;
    }
}

HRESULT Debugger::SetupStep(ICorDebugThread *pThread, Debugger::StepType stepType)
{
    HRESULT Status;

    ToRelease<ICorDebugStepper> pStepper;
    IfFailRet(pThread->CreateStepper(&pStepper));

    CorDebugIntercept mask = (CorDebugIntercept)(INTERCEPT_ALL & ~(INTERCEPT_SECURITY | INTERCEPT_CLASS_INIT));
    IfFailRet(pStepper->SetInterceptMask(mask));

    CorDebugUnmappedStop stopMask = STOP_NONE;
    IfFailRet(pStepper->SetUnmappedStopMask(stopMask));

    ToRelease<ICorDebugStepper2> pStepper2;
    IfFailRet(pStepper->QueryInterface(IID_ICorDebugStepper2, (LPVOID *)&pStepper2));

    IfFailRet(pStepper2->SetJMC(Debugger::IsJustMyCode()));

    if (stepType == STEP_OUT)
    {
        IfFailRet(pStepper->StepOut());
        return S_OK;
    }

    BOOL bStepIn = stepType == STEP_IN;

    COR_DEBUG_STEP_RANGE range;
    if (SUCCEEDED(Modules::GetStepRangeFromCurrentIP(pThread, &range)))
    {
        IfFailRet(pStepper->StepRange(bStepIn, &range, 1));
    } else {
        IfFailRet(pStepper->Step(bStepIn));
    }

    return S_OK;
}

HRESULT Debugger::StepCommand(int threadId,
                              StepType stepType)
{
    HRESULT Status;
    ToRelease<ICorDebugThread> pThread;
    IfFailRet(m_pProcess->GetThread(threadId, &pThread));
    DisableAllSteppers(m_pProcess);
    IfFailRet(SetupStep(pThread, stepType));
    IfFailRet(m_pProcess->Continue(0));
    return S_OK;
}

HRESULT Debugger::Continue()
{
    if (!m_pProcess)
        return E_FAIL;
    return m_pProcess->Continue(0);
}

HRESULT Debugger::Pause()
{
    if (!m_pProcess)
        return E_FAIL;
    HRESULT Status = m_pProcess->Stop(0);
    if (Status == S_OK)
        m_protocol->EmitStoppedEvent(StoppedEvent(StopPause, 0));
    return Status;
}

HRESULT Debugger::GetThreads(std::vector<Thread> &threads)
{
    if (!m_pProcess)
        return E_FAIL;
    return GetThreadsState(m_pProcess, threads);
}

HRESULT Debugger::SetBreakpoint(std::string filename, int linenum, Breakpoint &breakpoint)
{
    return InsertBreakpointInProcess(m_pProcess, filename, linenum, breakpoint);
}

HRESULT Debugger::GetStackTrace(int threadId, int lowFrame, int highFrame, std::vector<StackFrame> &stackFrames)
{
    HRESULT Status;
    if (!m_pProcess)
        return E_FAIL;
    ToRelease<ICorDebugThread> pThread;
    IfFailRet(m_pProcess->GetThread(threadId, &pThread));
    return ::GetStackTrace(pThread, lowFrame, highFrame, stackFrames);
}

HRESULT MIProtocol::StepCommand(const std::vector<std::string> &args,
                                std::string &output,
                                Debugger::StepType stepType)
{
    HRESULT Status;
    DWORD threadId = GetIntArg(args, "--thread", GetLastStoppedThreadId());
    m_debugger->StepCommand(threadId, stepType);
    output = "^running";
    return S_OK;
}

static std::string AddrToString(uint64_t addr)
{
    std::stringstream ss;
    ss << "0x" << std::setw(2 * sizeof(void*)) << std::setfill('0') << std::hex << addr;
    return ss.str();
}

HRESULT PrintFrameLocation(const StackFrame &stackFrame, std::string &output)
{
    HRESULT Status;

    std::stringstream ss;

    if (!stackFrame.source.IsNull())
    {
        ss << "file=\"" << MIProtocol::EscapeMIValue(stackFrame.source.name) << "\","
           << "fullname=\"" << MIProtocol::EscapeMIValue(stackFrame.source.path) << "\","
           << "line=\"" << stackFrame.line << "\","
           << "col=\"" << stackFrame.column << "\","
           << "end-line=\"" << stackFrame.endLine << "\","
           << "end-col=\"" << stackFrame.endColumn << "\",";
    }

    if (stackFrame.clrAddr.methodToken != 0)
    {
        ss << "clr-addr={module-id=\"{" << stackFrame.moduleId << "}\","
           << "method-token=\"0x"
           << std::setw(8) << std::setfill('0') << std::hex << stackFrame.clrAddr.methodToken << "\","
           << "il-offset=\"" << std::dec << stackFrame.clrAddr.ilOffset
           << "\",native-offset=\"" << stackFrame.clrAddr.nativeOffset << "\"},";
    }

    ss << "func=\"" << stackFrame.name << "\"";
    if (stackFrame.id != 0)
        ss << ",addr=\"" << AddrToString(stackFrame.addr) << "\"";

    output = ss.str();

    return stackFrame.source.IsNull() ? S_FALSE : S_OK;
}

HRESULT MIProtocol::PrintFrames(int threadId, std::string &output, int lowFrame, int highFrame)
{
    HRESULT Status;
    std::stringstream ss;

    std::vector<StackFrame> stackFrames;
    IfFailRet(m_debugger->GetStackTrace(threadId, lowFrame, highFrame, stackFrames));

    int currentFrame = lowFrame;

    ss << "stack=[";
    const char *sep = "";

    for (const StackFrame &stackFrame : stackFrames)
    {
        ss << sep;
        sep = ",";

        std::string frameLocation;
        PrintFrameLocation(stackFrame, frameLocation);

        ss << "frame={level=\"" << currentFrame << "\"";
        if (!frameLocation.empty())
            ss << "," << frameLocation;
        ss << "}";
        currentFrame++;
    }

    ss << "]";

    output = ss.str();

    return S_OK;
}

HRESULT MIProtocol::PrintVariables(const std::vector<Variable> &variables, std::string &output)
{
    const bool printValues = true;
    const bool printTypes = false;

    HRESULT Status;

    std::stringstream ss;
    ss << "variables=[";
    const char *sep = "";

    for (const Variable &var : variables)
    {
        ss << sep;
        sep = ",";

        ss << "{name=\"" << EscapeMIValue(var.name) << "\"";
        ss << ",value=\"" << EscapeMIValue(var.value) << "\"";
        ss << "}";
    }

    ss << "]";
    output = ss.str();
    return S_OK;
}

void MIProtocol::EmitStoppedEvent(StoppedEvent event)
{
    HRESULT Status;

    std::string frameLocation;
    PrintFrameLocation(event.frame, frameLocation);

    switch(event.reason)
    {
        case StopBreakpoint:
        {
            MIProtocol::Printf("*stopped,reason=\"breakpoint-hit\",thread-id=\"%i\",stopped-threads=\"all\",bkptno=\"%u\",times=\"%u\",frame={%s}\n",
                event.threadId, (unsigned int)event.breakpoint.id, (unsigned int)event.breakpoint.hitCount, frameLocation.c_str());
            return;
        }
        case StopStep:
        {
            MIProtocol::Printf("*stopped,reason=\"end-stepping-range\",thread-id=\"%i\",stopped-threads=\"all\",frame={%s}\n",
                event.threadId, frameLocation.c_str());
            return;
        }
        case StopException:
        {
            std::string category = "clr";
            std::string stage = "unhandled";
            MIProtocol::Printf("*stopped,reason=\"exception-received\",exception-name=\"%s\",exception=\"%s\",exception-stage=\"%s\",exception-category=\"%s\",thread-id=\"%i\",stopped-threads=\"all\",frame={%s}\n",
                event.text.c_str(),
                MIProtocol::EscapeMIValue(event.description).c_str(),
                stage.c_str(),
                category.c_str(),
                event.threadId,
                frameLocation.c_str());
            return;
        }
        case StopPause:
        {
            MIProtocol::Printf("*stopped,reason=\"interrupted\",stopped-threads=\"all\"\n");
            return;
        }
        default:
            break;
    }
}

void MIProtocol::EmitExitedEvent(ExitedEvent event)
{
    MIProtocol::Printf("*stopped,reason=\"exited\",exit-code=\"%i\"\n", event.exitCode);
}

void MIProtocol::EmitThreadEvent(ThreadEvent event)
{
    const char *reasonText = "";
    switch(event.reason)
    {
        case ThreadStarted:
            reasonText = "thread-created";
            break;
        case ThreadExited:
            reasonText = "thread-exited";
            break;
    }
    MIProtocol::Printf("=%s,id=\"%i\"\n", reasonText, event.threadId);
}

void MIProtocol::EmitOutputEvent(OutputEvent event)
{
    if (event.source.empty())
        MIProtocol::Printf("=message,text=\"%s\",send-to=\"output-window\"\"\n",
            MIProtocol::EscapeMIValue(event.output).c_str());
    else
        MIProtocol::Printf("=message,text=\"%s\",send-to=\"output-window\",source=\"%s\"\n",
            MIProtocol::EscapeMIValue(event.output).c_str(),
            MIProtocol::EscapeMIValue(event.source).c_str());
}

HRESULT MIProtocol::HandleCommand(std::string command,
                                  const std::vector<std::string> &args,
                                  std::string &output)
{
    static std::unordered_map<std::string, CommandCallback> commands {
    { "thread-info", [this](ICorDebugProcess *, const std::vector<std::string> &, std::string &output){
        HRESULT Status = S_OK;

        std::vector<Thread> threads;
        IfFailRet(m_debugger->GetThreads(threads));

        std::stringstream ss;

        ss << "threads=[";

        const char *sep = "";
        for (const Thread& thread : threads)
        {
            ss << "{id=\"" << thread.id
            << "\",name=\"" << MIProtocol::EscapeMIValue(thread.name) << "\",state=\""
            << (thread.running ? "running" : "stopped") << "\"}" << sep;
            sep = ",";
        }

        ss << "]";
        output = ss.str();
        return S_OK;
    } },
    { "exec-continue", [this](ICorDebugProcess *, const std::vector<std::string> &, std::string &output){
        HRESULT Status;
        IfFailRet(m_debugger->Continue());
        output = "^running";
        return S_OK;
    } },
    { "exec-interrupt", [this](ICorDebugProcess *, const std::vector<std::string> &, std::string &output){
        HRESULT Status;
        IfFailRet(m_debugger->Pause());
        output = "^done";
        return S_OK;
    } },
    { "break-insert", [this](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        std::string filename;
        unsigned int linenum;
        ULONG32 id;
        Breakpoint breakpoint;
        if (ParseBreakpoint(args, filename, linenum)
            && SUCCEEDED(m_debugger->SetBreakpoint(filename, linenum, breakpoint)))
        {
            PrintBreakpoint(breakpoint, output);
            return S_OK;
        }

        output = "Unknown breakpoint location format";
        return E_FAIL;
    } },
    { "break-delete", [](ICorDebugProcess *, const std::vector<std::string> &args, std::string &) -> HRESULT {
        for (const std::string &idStr : args)
        {
            bool ok;
            int id = ParseInt(idStr, ok);
            if (ok)
                DeleteBreakpoint(id);
        }
        return S_OK;
    } },
    { "exec-step", [this](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        return StepCommand(args, output, Debugger::STEP_IN);
    }},
    { "exec-next", [this](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        return StepCommand(args, output, Debugger::STEP_OVER);
    }},
    { "exec-finish", [this](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        return StepCommand(args, output, Debugger::STEP_OUT);
    }},
    { "exec-abort", [this](ICorDebugProcess *, const std::vector<std::string> &, std::string &output) -> HRESULT {
        m_debugger->TerminateProcess();
        return S_OK;
    }},
    { "target-attach", [this](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        HRESULT Status;
        if (args.size() != 1)
        {
            output = "Command requires an argument";
            return E_INVALIDARG;
        }
        bool ok;
        int pid = ParseInt(args.at(0), ok);
        if (!ok) return E_INVALIDARG;
        IfFailRet(m_debugger->AttachToProcess(pid));
        // TODO: print succeessful result
        return S_OK;
    }},
    { "target-detach", [this](ICorDebugProcess *, const std::vector<std::string> &, std::string &output) -> HRESULT {
        m_debugger->DetachFromProcess();
        return S_OK;
    }},
    { "stack-list-frames", [this](ICorDebugProcess *, const std::vector<std::string> &args_orig, std::string &output) -> HRESULT {
        std::vector<std::string> args = args_orig;
        DWORD threadId = GetIntArg(args, "--thread", GetLastStoppedThreadId());
        int lowFrame = 0;
        int highFrame = INT_MAX;
        StripArgs(args);
        GetIndices(args, lowFrame, highFrame);
        return PrintFrames(threadId, output, lowFrame, highFrame);
    }},
    { "stack-list-variables", [this](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        HRESULT Status;

        StackFrame stackFrame(GetIntArg(args, "--thread", GetLastStoppedThreadId()), GetIntArg(args, "--frame", 0), "");
        std::vector<Scope> scopes;
        std::vector<Variable> variables;
        IfFailRet(m_debugger->GetScopes(stackFrame.id, scopes));
        if (!scopes.empty() && scopes[0].variablesReference != 0)
        {
            IfFailRet(m_debugger->GetVariables(scopes[0].variablesReference, VariablesNamed, 0, 0, variables));
        }

        PrintVariables(variables, output);

        return S_OK;
    }},
    { "var-create", [](ICorDebugProcess *pProcess, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (!pProcess) return E_FAIL;
        HRESULT Status;

        if (args.size() < 2)
        {
            output = "Command requires at least 2 arguments";
            return E_FAIL;
        }

        ToRelease<ICorDebugThread> pThread;
        DWORD threadId = GetIntArg(args, "--thread", GetLastStoppedThreadId());
        IfFailRet(pProcess->GetThread(threadId, &pThread));

        ToRelease<ICorDebugFrame> pFrame;
        IfFailRet(GetFrameAt(pThread, GetIntArg(args, "--frame", 0), &pFrame));

        std::string var_name = args.at(0);
        std::string var_expr = args.at(1);
        if (var_expr == "*" && args.size() >= 3)
            var_expr = args.at(2);

        return CreateVar(pThread, pFrame, var_name, var_expr, output);
    }},
    { "var-list-children", [](ICorDebugProcess *pProcess, const std::vector<std::string> &args_orig, std::string &output) -> HRESULT {
        if (!pProcess) return E_FAIL;
        HRESULT Status;

        std::vector<std::string> args = args_orig;

        int print_values = 0;
        int var_index = 0;
        if (!args.empty())
        {
            auto first_arg_it = args.begin();
            if (*first_arg_it == "1" || *first_arg_it == "--all-values")
            {
                print_values = 1;
                args.erase(first_arg_it);
            }
            else if (*first_arg_it == "2" || *first_arg_it == "--simple-values")
            {
                print_values = 2;
                args.erase(first_arg_it);
            }
        }

        if (args.empty())
        {
            output = "Command requires an argument";
            return E_FAIL;
        }

        ToRelease<ICorDebugThread> pThread;
        DWORD threadId = GetIntArg(args, "--thread", GetLastStoppedThreadId());
        IfFailRet(pProcess->GetThread(threadId, &pThread));

        ToRelease<ICorDebugFrame> pFrame;
        pThread->GetActiveFrame(&pFrame);
        int childStart = 0;
        int childEnd = INT_MAX;
        StripArgs(args);
        GetIndices(args, childStart, childEnd);
        return ListChildren(childStart, childEnd, args.at(var_index), print_values, pThread, pFrame, output);
    }},
    { "var-delete", [](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (args.size() < 1)
        {
            output = "Command requires at least 1 argument";
            return E_FAIL;
        }
        return DeleteVar(args.at(0));
    }},
    { "gdb-exit", [this](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        this->m_exit = true;

        m_debugger->TerminateProcess();

        return S_OK;
    }},
    { "file-exec-and-symbols", [this](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (args.empty())
            return E_INVALIDARG;
        m_fileExec = args.at(0);
        return S_OK;
    }},
    { "exec-arguments", [this](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        m_execArgs = args;
        return S_OK;
    }},
    { "exec-run", [this](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        HRESULT Status = m_debugger->RunProcess(m_fileExec, m_execArgs);
        if (SUCCEEDED(Status))
            output = "^running";
        return Status;
    }},
    { "environment-cd", [this](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (args.empty())
            return E_INVALIDARG;
        return SetWorkDir(args.at(0)) ? S_OK : E_FAIL;
    }},
    { "handshake", [](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (!args.empty() && args.at(0) == "init")
            output = "request=\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\"";

        return S_OK;
    }},
    { "gdb-set", [this](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (args.size() == 2)
        {
            if (args.at(0) == "just-my-code")
            {
                Debugger::SetJustMyCode(args.at(1) == "1");
            }
        }
        return S_OK;
    }},
    { "interpreter-exec", [this](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        return S_OK;
    }},
    { "break-exception-insert", [](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        if (args.empty())
            return E_FAIL;
        size_t i = 1;
        if (args.at(0) == "--mda")
            i = 2;

        std::stringstream ss;
        const char *sep = "";
        ss << "bkpt=[";
        for (; i < args.size(); i++)
        {
            Breakpoint b;
            InsertExceptionBreakpoint(args.at(i), b);
            ss << sep;
            sep = ",";
            ss << "{number=\"" << b.id << "\"}";
        }
        ss << "]";
        output = ss.str();

        return S_OK;
    }},
    { "var-show-attributes", [](ICorDebugProcess *, const std::vector<std::string> &args, std::string &output) -> HRESULT {
        output = "status=\"noneditable\"";
        return S_OK;
    }},
    };

    auto command_it = commands.find(command);

    if (command_it == commands.end())
    {
        output = "Unknown command: " + command;
        return E_FAIL;
    }

    return command_it->second(m_debugger->GetProcess(), args, output);
}

static std::vector<std::string> TokenizeString(const std::string &str, const char *delimiters = " \t\n\r")
{
    enum {
        StateSpace,
        StateToken,
        StateQuotedToken,
        StateEscape
    } state = StateSpace;

    std::vector<std::string> result;

    for (char c : str)
    {
        switch(state)
        {
            case StateSpace:
                if (strchr(delimiters, c))
                    continue;
                result.emplace_back();
                state = c == '"' ? StateQuotedToken : StateToken;
                if (state != StateQuotedToken)
                    result.back() +=c;
                break;
            case StateToken:
                if (strchr(delimiters, c))
                    state = StateSpace;
                else
                    result.back() += c;
                break;
            case StateQuotedToken:
                if (c == '\\')
                    state = StateEscape;
                else if (c == '"')
                    state = StateSpace;
                else
                    result.back() += c;
                break;
            case StateEscape:
                result.back() += c;
                state = StateQuotedToken;
                break;
        }
    }
    return result;
}

static bool ParseLine(const std::string &str,
                      std::string &token,
                      std::string &cmd,
                      std::vector<std::string> &args)
{
    token.clear();
    cmd.clear();
    args.clear();

    std::vector<std::string> result = TokenizeString(str);

    if (result.empty())
        return false;

    auto cmd_it = result.begin();

    std::size_t i = cmd_it->find_first_not_of("0123456789");
    if (i == std::string::npos)
        return false;

    if (cmd_it->at(i) != '-')
        return false;

    token = cmd_it->substr(0, i);
    cmd = cmd_it->substr(i + 1);
    result.erase(result.begin());
    args = result;

    return true;
}

void MIProtocol::CommandLoop()
{
    static char inputBuffer[1024];
    std::string token;

    while (!m_exit)
    {
        token.clear();

        Printf("(gdb)\n");
        if (!fgets(inputBuffer, _countof(inputBuffer), stdin))
            break;

        std::vector<std::string> args;
        std::string command;
        if (!ParseLine(inputBuffer, token, command, args))
        {
            Printf("%s^error,msg=\"Failed to parse input\"\n", token.c_str());
            continue;
        }

        std::string output;
        HRESULT hr = HandleCommand(command, args, output);

        if (m_exit)
            break;

        if (SUCCEEDED(hr))
        {
            const char *resultClass;
            if (output.empty())
                resultClass = "^done";
            else if (output.at(0) == '^')
                resultClass = "";
            else
                resultClass = "^done,";

            Printf("%s%s%s\n", token.c_str(), resultClass, output.c_str());
        }
        else
        {
            const char *sep = output.empty() ? "" : " ";
            Printf("%s^error,msg=\"Error: 0x%08x%s%s\"\n", token.c_str(), hr, sep, output.c_str());
        }
    }

    if (!m_exit)
        m_debugger->TerminateProcess();

    Printf("%s^exit\n", token.c_str());
}
