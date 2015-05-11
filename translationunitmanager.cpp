
#include "translationunitmanager.h"
#include <manager.h>
#include <logmanager.h>
#include <editormanager.h>
#include <cbeditor.h>
#include <cbproject.h>
#include "macrosmanager.h"
#include <cbstyledtextctrl.h>

#include <llvm/Support/Host.h>
#include <llvm/Support/MemoryBuffer.h>
#include <clang/Basic/TargetInfo.h>
#include <clang/Parse/ParseAST.h>
#include <clang/Lex/HeaderSearch.h>
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/Utils.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Lex/Preprocessor.h>



#include "ccevents.h"
#include "clangcclogger.h"
#include "clang_cc.h"
#include "util.h"
#include "diagnosticprinter.h"
#include "options.h"





using namespace clang;
TranslationUnitManager::TranslationUnitManager(ClangCC& CC):
    m_CC(CC)
{

}

bool TranslationUnitManager::AddASTUnitForProjectFile(ProjectFile* file, ASTUnit* tu)
{
    cbAssert (file && file->file.FileExists() && "File does not exist.");
    cbProject* proj = file->GetParentProject();

    if (proj)
    {
        std::lock_guard<std::mutex> lock(m_ProjectsMapMutex);
        m_ProjectTranslationUnits[proj].insert(std::make_pair(file->file.GetFullPath(),
                                               std::shared_ptr<ASTUnit>(tu)));
        return true;
    }
    return false;
}

ASTUnit* TranslationUnitManager::ParseProjectFile(ProjectFile* file,bool allowAdd)
{
    cbAssert (file && file->file.FileExists() && "File not exists");


    wxString fileName = file->file.GetFullPath();
    auto project = file->GetParentProject();

    DiagnosticOptions* diagOpts = new DiagnosticOptions();
    IntrusiveRefCntPtr<DiagnosticsEngine> diags = CompilerInstance::createDiagnostics(diagOpts,0);

    auto iter = m_CompilationDatabases.find(project);

    if(iter == m_CompilationDatabases.end())
    {
        LoggerAccess::Get()->Log(wxT("Could not find the compilation database for project : ") + project->GetTitle());
        return nullptr;
    }
    auto compileCommands = iter->second->getCompileCommands(wx2std(file->file.GetFullPath()));
    if (compileCommands.empty())
    {
        LoggerAccess::Get()->Log(wxT("No compile commands for the file") + file->file.GetName());
        return nullptr;
    }

    auto args = compileCommands[0].CommandLine;
#ifdef CLANGCC_DEBUG_LOGGING
    wxString commandLine = wxT("Command Line is ");
    std::for_each(args.begin(),args.end(), [&](const std::string s){ commandLine.Append(s); commandLine.append(' '); });
    LoggerAccess::Get()->Log(commandLine);
#endif


    args.push_back("-fsyntax-only");
    if (!Options::Get().ShouldSpellCheck())
        args.push_back("-fno-spell-checking");

    {   //if the file is already being parsed return immediately.
        std::lock_guard<std::mutex> lock(m_FilesBeingParsedMutex);
        auto it = std::find(m_FilesBeingParsed.begin(), m_FilesBeingParsed.end(), file);
        if (it != m_FilesBeingParsed.end())
            return nullptr;
        m_FilesBeingParsed.push_back(file);
    }

    //Parsing started event.
    ccEvent startEvent(ccEVT_PARSE_START, fileName, nullptr, file);
    AddPendingEvent(startEvent);
#ifdef CLANGCC_TIMING
    wxStopWatch watch;
#endif // CLANGCC_TIMING

    std::vector<const char*> argsinChar;
    argsinChar.reserve(args.size());
    std::transform(args.begin(), args.end(), std::back_inserter(argsinChar),
                        [](const std::string& strings){return strings.c_str();});

    CompilerInvocation* invocation = clang::createInvocationFromCommandLine(llvm::makeArrayRef(argsinChar),
                                                                           diags);


    PreprocessorOptions &ppOpts = invocation->getPreprocessorOpts();
    ppOpts.RemappedFilesKeepOriginalName = true;
    ppOpts.AllowPCHWithCompilerErrors = true;

    invocation->getFrontendOpts().SkipFunctionBodies = Options::Get().ShouldSkipFunctionBodies();


    auto ast = ASTUnit::LoadFromCompilerInvocation(invocation,diags,
                                                       true, /* OnlyLocalDecls */
                                                       true, /*CaptureDiagnostics*/
                                                       true, /*PrecompilePreamble*/
                                                       TU_Complete,
                                                       true,/*CacheCodeCompilationResults*/
                                                       true,/* Include Brief Comment*/
                                                       true  /* User Files are volatile*/
                                                       ).release();

#ifdef CLANGCC_TIMING
    LoggerAccess::Get()->Log(wxString::Format(_("Parsing %s completed in %ldms"), file->file.GetFullName().c_str(), watch.Time()),Logger::info);
#endif // CLANGCC_TIMING
    if (!ast)
      LoggerAccess::Get()->Log(_("\t Cannot Create ASTUnit for ") + file->file.GetFullName());
    if (ast)
    {
        if (allowAdd)
            AddASTUnitForProjectFile(file,ast);
    }

    {   //File is free again
        std::lock_guard<std::mutex> lock(m_FilesBeingParsedMutex);
        m_FilesBeingParsed.erase(std::remove(m_FilesBeingParsed.begin(), m_FilesBeingParsed.end(), file));
    }
    //Parsing ended event
    ccEvent endEvent(ccEVT_PARSE_END, fileName, ast, file);
    AddPendingEvent(endEvent);

    return ast;
}

ASTUnit* TranslationUnitManager::GetASTUnitForProjectFile(ProjectFile* file)
{
    cbAssert (file && file->file.FileExists() && "File not exists");

    cbProject* proj = file->GetParentProject();
    wxString fileName = file->file.GetFullPath();
    bool isHeader = IsHeaderFile(fileName);
    std::lock_guard<std::mutex> lock(m_ProjectsMapMutex);
    auto it = m_ProjectTranslationUnits.find(proj);
    if (it != m_ProjectTranslationUnits.end())
    {
        auto& fileMap =(*it).second;
        auto it = fileMap.find(fileName);
        if ( it != fileMap.end())
            return (*it).second.get();
        // if it's a header file check whether an AST exists for sourcefile with the same base name .
        if (isHeader)
        {
            ProjectFile* sourceFile = GetProjectFilePair(file);
            if (sourceFile)
            {
                it = fileMap.find(sourceFile->file.GetFullPath());
                if (it != fileMap.end())
                    return (*it).second.get();
            }
        }
    }
    return nullptr;
}

ASTUnit* TranslationUnitManager::ReparseProjectFile(ProjectFile* file)
{
    {   //if the file is already being parsed return immediately.
        std::lock_guard<std::mutex> lock(m_FilesBeingParsedMutex);
        auto it = std::find(m_FilesBeingParsed.begin(), m_FilesBeingParsed.end(), file);
        if (it != m_FilesBeingParsed.end())
            return nullptr;
        m_FilesBeingParsed.push_back(file);
    }

    wxString fileName = file->file.GetFullPath();

    ccEvent startEvent(ccEVT_REPARSE_START, fileName, nullptr, file);
    AddPendingEvent(startEvent);

#ifdef CLANGCC_TIMING
    wxStopWatch watch;
#endif // CLANGCC_TIMING

    cbStyledTextCtrl* control = Manager::Get()->GetEditorManager()->GetBuiltinEditor(fileName)->GetControl();
    SmallVector<ASTUnit::RemappedFile,1> remappedFiles;


    if (control->GetModify())
    {
        unsigned length = control->GetLength();
        llvm::MemoryBuffer* membuf = llvm::MemoryBuffer::getNewUninitMemBuffer(length+1,wx2std(fileName)).release();
        control->SendMsg(SCI_GETTEXT, length+1, (wxUIntPtr)membuf->getBufferStart());
        ASTUnit::RemappedFile remap = std::make_pair(wx2std(fileName),membuf);
        remappedFiles.push_back(remap);
    }

    ASTUnit* tu = GetASTUnitForProjectFile(file);
    if (!tu)
        tu = ParseProjectFile(file);

    if (!tu || tu->Reparse(remappedFiles))
         LoggerAccess::Get()->Log(_("\t Reparsing Failed : ")+ file->file.GetFullName());

#ifdef CLANGCC_TIMING
    LoggerAccess::Get()->Log(wxString::Format(_("Reparsing completed in %ldms"), watch.Time()), Logger::info);
#endif // CLANGCC_TIMING

    {   //File is free again
        std::lock_guard<std::mutex> lock(m_FilesBeingParsedMutex);
        m_FilesBeingParsed.erase(std::remove(m_FilesBeingParsed.begin(), m_FilesBeingParsed.end(), file));
    }

    ccEvent endEvent(ccEVT_REPARSE_END, fileName, tu, file);
    AddPendingEvent(endEvent);
    return tu;

}

bool TranslationUnitManager::IsFileBeingParsed(ProjectFile* file)
{
    std::lock_guard<std::mutex> lock(m_FilesBeingParsedMutex);
    auto it = std::find(m_FilesBeingParsed.begin(), m_FilesBeingParsed.end(), file);
    if (it != m_FilesBeingParsed.end())
        return true;
    return false;
}
std::vector<ASTMemoryUsage> TranslationUnitManager::GetMemoryUsageForProjectFile(ProjectFile* file)
{
    ASTUnit* tu = GetASTUnitForProjectFile(file);
    std::vector<ASTMemoryUsage> usages;
    if (tu)
    {
        //AST Context
        ASTContext& ctx = tu->getASTContext();
        usages.emplace_back(AST_Nodes, ctx.getASTAllocatedMemory());
        usages.emplace_back(AST_Identifiers, ctx.Idents.getAllocator().getTotalMemory());
        usages.emplace_back(AST_Selectors, ctx.Selectors.getTotalMemory());
        usages.emplace_back(AST_SideTables, ctx.getSideTableAllocatedMemory());

        //Source Manager
        usages.emplace_back(SM_ContentCache, ctx.getSourceManager().getContentCacheSize());
        const SourceManager::MemoryBufferSizes& srcBufs = tu->getSourceManager().getMemoryBufferSizes();
        usages.emplace_back(SM_Malloc, srcBufs.malloc_bytes);
        usages.emplace_back(SM_Mmap, srcBufs.mmap_bytes);
        usages.emplace_back(SM_DataStructures, tu->getSourceManager().getDataStructureSizes());
        // Preprocessor
        Preprocessor& PP = tu->getPreprocessor();
        usages.emplace_back(PP_Total, PP.getTotalMemory());
        usages.emplace_back(PP_HeaderSearch, PP.getHeaderSearchInfo().getTotalMemory());
        if (PP.getPreprocessorOpts().DetailedRecord)
            usages.emplace_back(ASTMemoryUsage(PP_PreprocessingRecord, PP.getPreprocessingRecord()->getTotalMemory()));
    }
    return usages;
}

void TranslationUnitManager::Clear()
{
    std::lock_guard<std::mutex> lock(m_ProjectsMapMutex);
    m_ProjectTranslationUnits.clear();
}

void TranslationUnitManager::RemoveFile(cbProject* project,const wxString& fileName)
{
    std::lock_guard<std::mutex> lock(m_ProjectsMapMutex);
    //Not many projects so no worries
    for (auto & it : m_ProjectTranslationUnits)
    {
        if(it.first == project)
            it.second.erase(fileName);
    }
}

void TranslationUnitManager::RemoveProject(cbProject* project)
{
    std::lock_guard<std::mutex> lock(m_ProjectsMapMutex);
    m_ProjectTranslationUnits.erase(project);
    m_CompilationDatabases.erase(project);
}
void TranslationUnitManager::OnProjectOpened(CodeBlocksEvent& event)
{
	cbProject* project = event.GetProject();
	std::string errorMessage;
	auto database = clang::tooling::CompilationDatabase::loadFromDirectory(wx2std(project->GetBasePath()),errorMessage);
	if (database == nullptr)
	{
        LoggerAccess::Get()->Log(std2wx(errorMessage));
	}
	else
	{
        m_CompilationDatabases.insert(std::make_pair(project,std::move(database)));
    }
}

