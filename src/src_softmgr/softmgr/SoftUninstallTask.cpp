#include "stdafx.h"
#include "SoftUninstallTask.h"
#include "SoftUninstall.h"
#include "SoftUninstallApi.h"
#include "SoftUninstallSql.h"
#include "SoftUninstallEnum.h"
#include <skylark2/bkdb.h>
#include <tinyxml/tinyxml.h>
#include <libheader/libheader.h>
using namespace Skylark;
using namespace conew;
#include <stlsoft/algorithms/unordered.hpp>
#include <stlsoft/string/case_functions.hpp>
#include <stlsoft/smartptr/scoped_handle.hpp>
using namespace stlsoft;
#include <winstl/system/pid_sequence.hpp>
#include <winstl/shims/conversion/to_uint64.hpp>
#include <winstl/filesystem/filesystem_traits.hpp>
#include <winstl/filesystem/findfile_sequence.hpp>
using namespace winstl;
#include <locale>
#include <clocale>
#include <sstream>
#include <algorithm>
using namespace std;
using namespace stdext;
#include <Shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#include <ShObjIdl.h>
#include <ShlGuid.h>
#pragma comment(lib, "shell32.lib")
#include <Psapi.h>
#pragma comment(lib, "psapi.lib")
#include <tlhelp32.h>

namespace ksm
{

static inline void FileTimeToUnixTime(LPFILETIME pft, __time32_t* pt);
static inline int AtoiHelper(LPCSTR pStr, int def = 0);
static inline LPCSTR TiGetTextHelper(TiXmlElement *pElem, LPCSTR pName);
//////////////////////////////////////////////////////////////////////////
BOOL CSoftTask::IsExited(conew::CTaskMgr *pMgr)
{
	BOOL ret = FALSE;
	if(_pSoftUninst->IsExited()) return TRUE;

	// 检查是否存在更高优先级任务
	// 若存在，则放弃执行，并重新排除
	pMgr->LockTaskQueue();

	CTaskMgr::TaskQueue &queue = pMgr->NaiveGetTaskQueue();
	if(!queue.empty() && (*queue.begin())->GetPriority() < GetPriority())
	{
		this->AddRef();
		// 清除同种类型的任务
		pMgr->NaiveClearKindTask(GetType());
		pMgr->NaiveInsertTask(this);

		ret = TRUE;
	}

	pMgr->UnlockTaskQueue();

	return ret;
}

BOOL CSoftInitTask::TaskProcess(CTaskMgr *pMgr)
{
	::CoInitialize(NULL);

	::SetThreadLocale(MAKELANGID(LANG_CHINESE,SUBLANG_CHINESE_SIMPLIFIED));

	setlocale(LC_ALL, "chs");
	try{ locale::global(locale("chs")); } catch(...) {}

	do
	{
		SoftData2List softData2List;
		CSoftDataEnum softDataEnum(softData2List);		

		ISQLiteComDatabase3 *pDB = _pSoftUninst->GetDBPtr();
		ISoftUnincallNotify *pNotify = _pSoftUninst->GetNotify();

		BOOL cacheIsValid = CacheIsValid( );
		if(cacheIsValid)
		{
			if(!LoadSoftUninstData(pDB, softData2List))
			{
				pNotify->SoftDataEvent(UE_Data_Failed, NULL);
				break;
			}
			if(_pSoftUninst->IsExited()) break;

			pNotify->SoftDataEvent(UE_Data_Loaded, &softDataEnum);
			if(_pSoftUninst->IsExited()) break;

			if(!_pSoftUninst->LoadPinYin())
			{
				pNotify->SoftDataEvent(UE_Data_Failed, NULL);
				break;
			}
		}
		else
		{
			CSearchSoftData(static_cast<SoftData2List&>(softData2List));
			if(_pSoftUninst->IsExited()) break;

			pNotify->SoftDataEvent(UE_Data_Loaded, &softDataEnum);
			if(_pSoftUninst->IsExited()) break;

			InitCacheDB(pDB);

			if(!ImportSoftUninstData())
			{
				pNotify->SoftDataEvent(UE_Data_Failed, NULL);
				break;
			}
			if(_pSoftUninst->IsExited()) break;

			if(!_pSoftUninst->LoadPinYin())
			{
				pNotify->SoftDataEvent(UE_Data_Failed, NULL);
				break;
			}
			if(_pSoftUninst->IsExited()) break;

			FixSoftData2AndSave(_pSoftUninst, softData2List);
			if(_pSoftUninst->IsExited()) break;

			pNotify->SoftDataEvent(UE_Update, &softDataEnum);
			if(_pSoftUninst->IsExited()) break;

			UpdateCacheFlag();
		}

		if(_pSoftUninst->IsExited()) break;
		pNotify->SoftDataEvent(UE_Data_Completed, &softDataEnum);

		if(_pSoftUninst->IsExited()) break;
		_pSoftUninst->Startup(cacheIsValid);
	}
	while(FALSE);

	return FALSE;
}

BOOL CSoftInitTask::CacheIsValid() const 
{
	BOOL ret = FALSE;

	do
	{
		CCacheFlagOpr cache(_pSoftUninst->GetDBPtr());

		ISQLiteComDatabase3 *pDB = _pSoftUninst->GetDBPtr();

		if( pDB )
		{
			static const LPCWSTR pSqlInsert = 
				L"insert into local_soft_list"
				L"(soft_key,"
				L"guid,"
				L"display_name,"
				L"main_path,"
				L"descript,"
				L"descript_reg,"
				L"info_url,"
				L"spell_whole,"
				L"spell_acronym,"
				L"icon_location,"
				L"uninstall_string,"
				L"logo_url,"
				L"size,"
				L"last_use,"
				L"type_id,"
				L"soft_id,"
				L"match_type,"
				L"pattern,"
				L"daycnt)"
				L"values(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";

			ISQLiteComStatement3* pStateInsert = NULL;
			pDB->PrepareStatement(pSqlInsert, &pStateInsert);

			if( pStateInsert == NULL )
				break;
		}

		ULONGLONG ftTime;
		wstring xmlUninst = _pSoftUninst->GetKSafePath() + SOFT_UNINST_DAT;
		if(!cache.Query(xmlUninst, ftTime))
			break;

		WIN32_FILE_ATTRIBUTE_DATA wfad = {0};
		::GetFileAttributesExW(xmlUninst.c_str(), GetFileExInfoStandard, &wfad);

		ULARGE_INTEGER largeTime;
		largeTime.LowPart = wfad.ftLastWriteTime.dwLowDateTime;
		largeTime.HighPart = wfad.ftLastWriteTime.dwHighDateTime;

		if(ftTime != largeTime.QuadPart)
			break;

		ret = TRUE;
	}
	while(FALSE);

	return ret; 
}

BOOL CSoftInitTask::ImportSoftUninstData() 
{
	BOOL ret = FALSE;

	do	
	{
		wstring path = _pSoftUninst->GetKSafePath() + SOFT_UNINST_DAT;

		BkDatLibHeader header;
		CDataFileLoader	loader;
		if(!loader.GetLibDatHeader(path.c_str(), header))
			break;

		BkDatLibContent cont;
		if(!loader.GetLibDatContent(path.c_str(), cont))
			break;

		TiXmlDocument plugins;
		if(plugins.Parse((char*)cont.pBuffer) == NULL)
			break;

		ISQLiteComDatabase3 *pDB = _pSoftUninst->GetDBPtr();
		CCacheTransaction transaction(pDB);
		CCacheSoftData cacheSoftData(pDB);

		TiXmlHandle hRoot(plugins.FirstChildElement("allsoft"));
		hRoot = hRoot.FirstChildElement("softwares").Element();
		for(TiXmlElement *pElem = hRoot.FirstChildElement("soft").Element(); pElem != NULL; pElem = pElem->NextSiblingElement())
		{
			CA2W pLogo(TiGetTextHelper(pElem, "softlogo"));
			CA2W pBrief(TiGetTextHelper(pElem, "brief"));
			CA2W pInfoUrl(TiGetTextHelper(pElem, "infourl"));
			CA2W pPattern(TiGetTextHelper(pElem, "pattern"));

			cacheSoftData.Insert(
				AtoiHelper(pElem->Attribute("softid")), 
				AtoiHelper(pElem->Attribute("typeid")), 
				AtoiHelper(pElem->Attribute("matchtype")), 
				pLogo, 
				pBrief, 
				pInfoUrl, 
				pPattern
				);
		}

		ret = TRUE;
	}
	while(FALSE);

	return ret; 
}

void CSoftInitTask::UpdateCacheFlag()
{
//	UpdateRegCacheFlag(_pSoftUninst->GetDBPtr());

	WIN32_FILE_ATTRIBUTE_DATA wfad = {0};
	wstring xmlUninst = _pSoftUninst->GetKSafePath() + SOFT_UNINST_DAT;
	::GetFileAttributesExW(xmlUninst.c_str(), GetFileExInfoStandard, &wfad);

	ULARGE_INTEGER largeTime;
	largeTime.LowPart = wfad.ftLastWriteTime.dwLowDateTime;
	largeTime.HighPart = wfad.ftLastWriteTime.dwHighDateTime;

	CCacheFlagOpr(_pSoftUninst->GetDBPtr()).Insert(xmlUninst, largeTime.QuadPart);
}

inline int AtoiHelper(LPCSTR pStr, int def)
{
	if(pStr == NULL)
		return def;
	else
		return ::atoi(pStr);
}

inline LPCSTR TiGetTextHelper(TiXmlElement *pElem, LPCSTR pName)
{
	TiXmlElement *ppElem = pElem->FirstChildElement(pName);
	if(ppElem != NULL) 
		return ppElem->GetText();
	else 
		return NULL;
}

void CSoftRefreshTask::LoadSoftData2Key(SoftData2List &softData2List)
{
	CComPtr<ISQLiteComResultSet3> pRs;
	HRESULT hr = _pSoftUninst->GetDBPtr()->ExecuteQuery(L"select soft_key,guid,display_name from local_soft_list", &pRs);
	if(!SUCCEEDED(hr)) return;

	while(!pRs->IsEof())
	{
		softData2List.push_back(SoftData2());

		softData2List.back()._mask = SDM_Key;
		softData2List.back()._key = pRs->GetAsString(L"soft_key");
		softData2List.back()._guid = pRs->GetAsString(L"guid");
		softData2List.back()._displayName = pRs->GetAsString(L"display_name");

		pRs->NextRow();
	}
}

void CSoftRefreshTask::DeleteSoftData2(const SoftData2List &softData2List)
{
	CComPtr<ISQLiteComStatement3> pState;
	HRESULT hr = _pSoftUninst->GetDBPtr()->PrepareStatement(L"delete from local_soft_list where soft_key=?;", &pState);
	if(!SUCCEEDED(hr)) return;

	CCacheTransaction trans(_pSoftUninst->GetDBPtr());

	SoftData2CIter end = softData2List.end();
	for(SoftData2CIter it = softData2List.begin(); it != end; ++it)
	{
		pState->Bind(1, it->_key.c_str());
		pState->ExecuteUpdate();

		pState->Reset();
	}
}

BOOL CSoftRefreshTask::TaskProcess(class conew::CTaskMgr *) 
{
	ISoftUnincallNotify *pNotify = _pSoftUninst->GetNotify();
	pNotify->SoftDataEvent(UE_Refresh_Begin, NULL);

	// 判断是否需要更新
//	if(Need2Refresh())
	{
		SoftData2List oldData;
		LoadSoftData2Key(oldData);

		SoftData2List newData;
		CSearchSoftData(static_cast<SoftData2List&>(newData));
		if(_pSoftUninst->IsExited()) return FALSE;

		SoftData2Iter end = newData.end();
		for(SoftData2Iter it = newData.begin(); it != end;)
		{
			SoftData2Iter it2 = find_if(oldData.begin(), oldData.end(), 
				SoftData2KeyFind(it->_key));

			// key名称与显示名称完全相同的，则认为已存在的
			if(it2 != oldData.end() && it->_displayName == it2->_displayName)
			{
				oldData.erase(it2);
				newData.erase(it++);
			}
			else
			{
				++it;
			}

			if(oldData.empty()) break;
		}

		// 删除的数据
		if(!oldData.empty())
		{
			DeleteSoftData2(oldData);

			CSoftDataEnum softDataEnum(oldData);
			pNotify->SoftDataEvent(UE_Delete, &softDataEnum);
		}

		// 更新的数据
		if(!newData.empty())
		{
			// 修正数据
			FixSoftData2AndSave(_pSoftUninst, newData);

			CSoftDataEnum softDataEnum(newData);
			pNotify->SoftDataEvent(UE_Add, &softDataEnum);			
		}

		// 更新注册表缓存标记
//		UpdateRegCacheFlag(_pSoftUninst->GetDBPtr());
	}

	pNotify->SoftDataEvent(UE_Refresh_End, NULL);
	return FALSE; 
}

BOOL CSoftRefreshTask::Need2Refresh()
{
	BOOL ret = TRUE;

	do
	{
		ULONGLONG nowInst, nowUninst1, nowUninst2;
		if(
			!GetKeyWriteTime(HKEY_LOCAL_MACHINE, L"software\\microsoft\\windows\\currentversion\\installer\\userdata", nowInst) ||
			!GetKeyWriteTime(HKEY_CURRENT_USER, L"software\\microsoft\\windows\\currentversion\\uninstall", nowUninst1) ||
			!GetKeyWriteTime(HKEY_LOCAL_MACHINE, L"software\\microsoft\\windows\\currentversion\\uninstall", nowUninst2)
			) 
			break;

		CCacheFlagOpr cache(_pSoftUninst->GetDBPtr());
		ULONGLONG oldInst, oldUninst1, oldUninst2;
		if(
			!cache.Query(L"02:\\software\\microsoft\\windows\\currentversion\\installer\\userdata", oldInst) || oldInst != nowInst ||
			!cache.Query(L"01:\\software\\microsoft\\windows\\currentversion\\uninstall", oldUninst1) || oldUninst1 != nowUninst1 ||
			!cache.Query(L"02:\\software\\microsoft\\windows\\currentversion\\uninstall", oldUninst2) || oldUninst2 != nowUninst2
			)
			break;

		ret = FALSE;
	}
	while(FALSE);

	return ret;
}

std::wstring CSoftLinkRefreshTask::CanonicalizePath(LPCWSTR pPath)
{
	// 扩展环境变量、长路径、小写
	wstring path;
	if(wcschr(pPath, L'%') != NULL)
	{
		path = ExpandEnvironString(pPath);
		path = MakeAbsolutePath(path);
	}
	else
	{
		path = MakeAbsolutePath(pPath);
	}

	return make_lower(path);
}

void CSoftLinkRefreshTask::ParseLinkList(const WStrList &linkList, WStrList &dirList)
{
	CLinkOpr linkOpr;
	if(!linkOpr.Initialize()) return;

	WIN32_FIND_DATAW wfd;
	wchar_t path[MAX_PATH];

	WStrListCIter end = linkList.end();
	for(WStrListCIter it = linkList.begin(); it != end; ++it)
	{
		if(!linkOpr.GetPath(it->c_str(), path, MAX_PATH, &wfd)) continue;

		wstring strPath = CanonicalizePath(path);
		if(PathFileExistsX64(strPath))
		{
			if((wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
				RemoveBackSlash(strPath);
			else
				RemoveFileSpec(strPath);

			dirList.push_back(strPath);
		}
		else if(path[0] != L'\0')
		{
			_rubbishList.push_back(SoftRubbish2());
			_rubbishList.back()._type = _linkType;
			_rubbishList.back()._data = *it;
		}
	}
}

BOOL CSoftLinkRefreshTask::GetProcessPath(DWORD pid, std::wstring &path)
{
	scoped_handle<HANDLE> hProgPro(::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid), ::CloseHandle);
	if(hProgPro.get() == NULL) FALSE;

	wchar_t buffer[MAX_PATH];
	if(0 == ::GetModuleFileNameExW(hProgPro.get(), NULL, buffer, MAX_PATH)) return FALSE;

	::PathRemoveFileSpecW(buffer);
	path = CanonicalizePath(buffer);
	return TRUE;
}

void CSoftLinkRefreshTask::EnumTray(WStrList &dirList)
{
	try
	{
		HWND hWinWnd = ::FindWindowW(L"Shell_TrayWnd", NULL);
		if(hWinWnd == NULL) return;

		HWND hNotifyWnd = ::FindWindowExW(hWinWnd, NULL, L"TrayNotifyWnd", NULL);
		if(hNotifyWnd == NULL) return;

		hNotifyWnd = ::FindWindowEx(hNotifyWnd, NULL, L"SysPager", NULL);
		if(hNotifyWnd == NULL) return;

		HWND hTrayWnd = ::FindWindowEx(hNotifyWnd, NULL, L"ToolbarWindow32", NULL);
		if(hTrayWnd == NULL) return;

		DWORD pid = 0;
		::GetWindowThreadProcessId(hTrayWnd, &pid);
		if(pid == 0) return;

		scoped_handle<HANDLE> hProcess(::OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ, 0, pid), ::CloseHandle);
		if(hProcess.get() == NULL) return;

		LPVOID pRemoteBuffer = ::VirtualAllocEx(hProcess.get(), 0, 4096, MEM_COMMIT, PAGE_READWRITE);
		if(pRemoteBuffer == 0) return;

		auto_buffer<char> pBuffer(4096);
		if(pBuffer.size() == NULL) return;

		LRESULT count = ::SendMessage(hTrayWnd, TB_BUTTONCOUNT, 0, 0);
		for(LRESULT i = 0; i < count; ++i)
		{
			LRESULT ret = ::SendMessage(hTrayWnd, TB_GETBUTTON, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(pRemoteBuffer));
			
			SIZE_T size;
			::ReadProcessMemory(hProcess.get(), pRemoteBuffer, &pBuffer[0], 4096, &size);

			HWND hProgWnd;
			PTBBUTTON pTBButton = reinterpret_cast<PTBBUTTON>(&pBuffer[0]);
			::ReadProcessMemory(hProcess.get(), reinterpret_cast<LPCVOID>(pTBButton->dwData), &hProgWnd, sizeof(hProgWnd), &size);

			DWORD pidProg = 0;
			::GetWindowThreadProcessId(hProgWnd, &pidProg);
			if(pid == 0) break;

			wstring path;
			if(!GetProcessPath(pidProg, path)) continue;
			dirList.push_back(path);
		}

		::VirtualFreeEx(hProcess.get(), pRemoteBuffer, 4096, MEM_RELEASE);
	}
	catch(...) {}
}

void CSoftLinkRefreshTask::EnumProcess(WStrList &dirList)
{
	try
	{
		pid_sequence pidList;
		for(pid_sequence::size_type i = 0; i < pidList.size(); ++i)
		{
			wstring path;
			if(!GetProcessPath(pidList[i], path)) continue;
			dirList.push_back(path);
		}
	}
	catch(...) {}
}

void CSoftLinkRefreshTask::ParseDstSoft(WStrList &dirList)
{
	WStrHash keyHash;
	CCacheSoftFind finder(_pSoftUninst->GetDBPtr());

	WStrListIter end = dirList.end();
	for(WStrListIter it = dirList.begin(); it != end; ++it)
	{
		wstring key;
		if(
			finder.Query(*it, key) &&
			keyHash.find(key) == keyHash.end()	// 过滤重复软件
			) 
		{
			_softList.push_back(key);
			keyHash.insert(key);
		}
	}
}

BOOL CSoftLinkRefreshTask::TaskProcess(class conew::CTaskMgr *) 
{
	ISoftUnincallNotify *pNotify = _pSoftUninst->GetNotify();
	pNotify->SoftLinkEvent(UE_Refresh_Begin, _linkType, NULL);

	//
	// 分析目标路径以及残留项
	//
	WStrList dirList;
	wchar_t path[MAX_PATH];

	if(_linkType == SIA_Start)
	{
		WStrList fileList;
		LinkFileEnum pred(fileList);

		::SHGetSpecialFolderPathW(NULL, path, CSIDL_STARTMENU, FALSE);
		::PathRemoveBackslashW(path);
		RecurseEnumFile(pred, path, L"*.*", 5);

		::SHGetSpecialFolderPath(NULL, path, CSIDL_COMMON_STARTMENU, FALSE);
		::PathFindExtensionW(path);
		RecurseEnumFile(pred, path, L"*.*", 5);

		ParseLinkList(fileList, dirList);
	}
	else if(_linkType == SIA_Quick)
	{
		WStrList fileList;
		LinkFileEnum pred(fileList);

		::SHGetSpecialFolderPathW(NULL, path, CSIDL_APPDATA, FALSE);
		::PathFindExtensionW(path);

		wcscat_s(path, MAX_PATH, L"\\Microsoft\\Internet Explorer\\Quick Launch");
		RecurseEnumFile(pred, path, L"*.*", 5);

		ParseLinkList(fileList, dirList);
	}
	else if(_linkType == SIA_Desktop)
	{
		WStrList fileList;
		LinkFileEnum pred(fileList);

		::SHGetSpecialFolderPathW(NULL, path, CSIDL_DESKTOPDIRECTORY, FALSE);
		::PathRemoveBackslashW(path);
		RecurseEnumFile(pred, path, L"*.*", 5);

		::SHGetSpecialFolderPath(NULL, path, CSIDL_COMMON_DESKTOPDIRECTORY, FALSE);
		::PathFindExtensionW(path);
		RecurseEnumFile(pred, path, L"*.*", 5);

		ParseLinkList(fileList, dirList);
	}
	else if(_linkType == SIA_Tray)
	{
		EnumTray(dirList);
	}
	else if(_linkType == SIA_Process)
	{
		EnumProcess(dirList);
	}
	else
	{
		assert(FALSE);
	}

	if(_pSoftUninst->IsExited()) return FALSE;
	dirList.erase(unordered_unique(dirList.begin(), dirList.end()), dirList.end());
	if(_pSoftUninst->IsExited()) return FALSE;

	//
	// 分析目标软件
	//
	ParseDstSoft(dirList);
	if(_pSoftUninst->IsExited()) return FALSE;

	//
	// 通知
	//
	if(!_rubbishList.empty())
	{
		// 保存到数据库中
		_pSoftUninst->GetDBPtr()->BeginTransaction();

		CCacheRubbish rubbish(_pSoftUninst->GetDBPtr());
		rubbish.Delete(_linkType);

		SoftRubbish2CIter end = _rubbishList.end();
		for(SoftRubbish2CIter it = _rubbishList.begin(); it != end; ++it)
		{
			rubbish.Insert(*it);
		}

		_pSoftUninst->GetDBPtr()->CommitTransaction();

		CSoftRubbishEnum softRubbishEnum(_rubbishList);
		pNotify->SoftRubbishEvent(UE_Update, &softRubbishEnum);
	}

	if(!_softList.empty())
	{
		// 保存到数据库中
		if(_linkType == SIA_Desktop || _linkType == SIA_Start || _linkType == SIA_Quick)
		{
			_pSoftUninst->GetDBPtr()->BeginTransaction();

			CCacheLink link(_pSoftUninst->GetDBPtr());
			link.Delete(_linkType);

			WStrListCIter end = _softList.end();
			for(WStrListCIter it = _softList.begin(); it != end; ++it)
			{
				link.Insert(_linkType, *it);
			}

			_pSoftUninst->GetDBPtr()->CommitTransaction();
		}

		CSoftLinkEnum softLinkEnum(_softList);
		pNotify->SoftLinkEvent(UE_Refresh_End, _linkType, &softLinkEnum);
	}
	else
	{
		pNotify->SoftLinkEvent(UE_Refresh_End, _linkType, NULL);
	}

	return FALSE; 
}

BOOL CSoftRubbishRefreshTask::TaskProcess(CTaskMgr *pMgr)
{
	ISoftUnincallNotify *pNotify = _pSoftUninst->GetNotify();
	pNotify->SoftRubbishEvent(UE_Refresh_Begin, NULL);

	WStrList linkList;
	LinkFileEnum pred(linkList);

	wchar_t path[MAX_PATH];
	SoftRubbish2List rubbish2List;

	//
	// 开始菜单
	//
	::SHGetSpecialFolderPathW(NULL, path, CSIDL_STARTMENU, FALSE);
	::PathRemoveBackslashW(path);
	RecurseEnumFile(pred, path, L"*.*", 5);

	::SHGetSpecialFolderPath(NULL, path, CSIDL_COMMON_STARTMENU, FALSE);
	::PathFindExtensionW(path);
	RecurseEnumFile(pred, path, L"*.*", 5);

	ParseLinkList(linkList, SIA_Start, rubbish2List);
	linkList.clear();

	if(_pSoftUninst->IsExited()) return FALSE;

	//
	// 快速启动
	//
	::SHGetSpecialFolderPathW(NULL, path, CSIDL_APPDATA, FALSE);
	::PathFindExtensionW(path);

	wcscat_s(path, MAX_PATH, L"\\Microsoft\\Internet Explorer\\Quick Launch");
	RecurseEnumFile(pred, path, L"*.*", 5);

	ParseLinkList(linkList, SIA_Quick, rubbish2List);
	linkList.clear();

	if(_pSoftUninst->IsExited()) return FALSE;

	//
	// 桌面
	//
	::SHGetSpecialFolderPathW(NULL, path, CSIDL_DESKTOPDIRECTORY, FALSE);
	::PathRemoveBackslashW(path);
	RecurseEnumFile(pred, path, L"*.*", 5);

	::SHGetSpecialFolderPath(NULL, path, CSIDL_COMMON_DESKTOPDIRECTORY, FALSE);
	::PathFindExtensionW(path);
	RecurseEnumFile(pred, path, L"*.*", 5);

	ParseLinkList(linkList, SIA_Desktop, rubbish2List);
	linkList.clear();

	if(_pSoftUninst->IsExited()) return FALSE;

	if(!rubbish2List.empty())
	{
		_pSoftUninst->GetDBPtr()->BeginTransaction();

		CCacheRubbish rubbish(_pSoftUninst->GetDBPtr());
		rubbish.Delete();

		SoftRubbish2CIter end = rubbish2List.end();
		for(SoftRubbish2CIter it = rubbish2List.begin(); it != end; ++it)
		{
			rubbish.Insert(*it);
		}

		_pSoftUninst->GetDBPtr()->CommitTransaction();

		CSoftRubbishEnum rubbishEnum(rubbish2List);
		pNotify->SoftRubbishEvent(UE_Refresh_End, &rubbishEnum);
	}
	else
	{
		pNotify->SoftRubbishEvent(UE_Refresh_End, NULL);
	}

	return FALSE;
}

void CSoftRubbishRefreshTask::ParseLinkList(const WStrList &linkList, SoftItemAttri type, SoftRubbish2List &rubbish2List)
{
	CLinkOpr linkOpr;
	if(!linkOpr.Initialize()) return;

	wchar_t path[MAX_PATH];
	WStrListCIter end = linkList.end();

	for(WStrListCIter it = linkList.begin(); it != end; ++it)
	{
		if(!linkOpr.GetPath(it->c_str(), path, MAX_PATH)) continue;

		// 标准化
		wstring strPath;
		if(wcschr(path, L'%') != NULL)
		{
			strPath = ExpandEnvironString(path);
			strPath = MakeAbsolutePath(strPath);
		}
		else
		{
			strPath = MakeAbsolutePath(path);
		}

		if(!PathFileExistsX64(strPath) && path[0] != L'\0')
		{
			rubbish2List.push_back(SoftRubbish2());
			rubbish2List.back()._type = type;
			rubbish2List.back()._data = *it;
		}
	}
}

CSoftRubbishSweep::CSoftRubbishSweep(CSoftUninstall *pSoftUnint)
: CSoftTask(pSoftUnint, TASK_TYPE_RUBBISH_SWEEP), _enum(_notifyList)
{ 
	SetPriority(TASK_PRI_RUBBISH_SWEEP); 

	_exited = FALSE;
	_recycle = TRUE;
	_hEvent = ::CreateEventW(NULL, TRUE, FALSE, NULL);
}

CSoftRubbishSweep::~CSoftRubbishSweep()
{
	::CloseHandle(_hEvent);
}


ISoftRubbishEnum* CSoftRubbishSweep::RubbishSweepingEnum()
{
	return &_enum;
}

void CSoftRubbishSweep::Uninitialize()
{
	IBaseTask::Release();
}

BOOL CSoftRubbishSweep::TaskProcess(class conew::CTaskMgr *) 
{
	ISoftUnincallNotify *pNotify = _pSoftUninst->GetNotify();

	pNotify->SoftRubbishSweepEvent(UE_Sweep_Begin, this);
	
	do
	{
		if(_exited) break;

		::WaitForSingleObject(_hEvent, INFINITE);
		if(_exited) break;

		pNotify->SoftRubbishSweepEvent(UE_Sweeping, this);

		_pSoftUninst->GetDBPtr()->BeginTransaction();

		SoftRubbish2CIter end = _delList.end();
		CCacheRubbish rubbish(_pSoftUninst->GetDBPtr());

		for(SoftRubbish2CIter it = _delList.begin(); !_exited && it != end; ++it)
		{
			DeleteFile2(it->_data, _recycle);
			rubbish.Delete(*it);

			_notifyList.clear();
			_notifyList.push_back(*it);

			pNotify->SoftRubbishSweepEvent(UE_Delete, this);
		}

		_pSoftUninst->GetDBPtr()->CommitTransaction();
	}
	while(FALSE);

	pNotify->SoftRubbishSweepEvent(UE_Sweep_End, this);
	return FALSE; 
}

BOOL CSoftRubbishSweep::RubbishSetSweep(PCSoftRubbish pcData)
{
	_delList.push_back(SoftRubbish2());
	_delList.back()._data = pcData->_pData;
	_delList.back()._type = pcData->_type;
	return TRUE;
}

BOOL CSoftRubbishSweep::RubbishSweep(BOOL recycle)
{
	_recycle = recycle;
	::SetEvent(_hEvent);
	return TRUE;
}

BOOL CSoftRubbishSweep::RubbishCancelSweep()
{
	_exited = TRUE;
	::SetEvent(_hEvent);
	return TRUE;
}

BOOL CSoftCalcSpaceTask::TaskProcess(conew::CTaskMgr *pMgr)
{
	if(_softData2List.empty())
	{
		CComPtr<ISQLiteComResultSet3> pRs;
		HRESULT hr = _pSoftUninst->GetDBPtr()->ExecuteQuery(L"select soft_key,main_path,size from local_soft_list where not main_path is null", &pRs);
		if(!SUCCEEDED(hr))
			return FALSE;

		while(!pRs->IsEof())
		{
			_softData2List.push_back(SoftData2());
			SoftData2 &softData2 = _softData2List.back();

			softData2._mask				= SDM_Key | SDM_Size;
			softData2._key				= pRs->GetAsString(L"soft_key");
			softData2._mainPath				= pRs->GetAsString(L"main_path");
			softData2._size				= pRs->GetInt64(L"size");

			pRs->NextRow();
		}

		_it = _softData2List.begin();
	}

	if(!_softData2List.empty())
	{
		_pSoftUninst->GetDBPtr()->BeginTransaction();

		CCacheCalcSpace calcSpace(_pSoftUninst->GetDBPtr());
		SoftData2Iter end = _softData2List.end();

		for(; _it != end; ++_it)
		{
			// 运算之前可以检测挂起任务
			if(IsExited(pMgr)) 
			{
				_pSoftUninst->GetDBPtr()->CommitTransaction();
				return FALSE;
			}

			_it->_size = CalcSpace(_it->_mainPath);

			// 运算之后只能检测系统退出
			if(_pSoftUninst->IsExited())
			{
				_pSoftUninst->GetDBPtr()->CommitTransaction();
				return FALSE;
			}

			calcSpace.Update(_it->_key, _it->_size);
		}

		_pSoftUninst->GetDBPtr()->CommitTransaction();

		CSoftDataEnum softDataEnum(_softData2List);
		_pSoftUninst->GetNotify()->SoftDataEvent(UE_Update, &softDataEnum);
	}
	return FALSE;
}

ULONGLONG CSoftCalcSpaceTask::CalcSpace(const std::wstring &root)
{
	try
	{
		ULONGLONG size = 0;
		findfile_sequence_w finder(root.c_str(), L"*.*");
		findfile_sequence_w::const_iterator end = finder.end();

		for(findfile_sequence_w::const_iterator it = finder.begin(); it != end; ++it)
		{
			findfile_sequence_w::const_iterator::value_type value = *(it);

			// 此处只能检测系统退出
			if(_pSoftUninst->IsExited()) return 0;

			if(value.is_directory())
			{
				size += CalcSpace(root + L'\\' + value.get_filename());
			}
			else
			{
				size += to_uint64(value.get_find_data());
			}
		}

		return size;
	}
	catch(...) {}

	return 0;
}

int CSoftLastUseTask::GetDayOver( CTime& timeCur, CTime& timeLast )
{
	if( timeLast.GetTime() > timeCur.GetTime() )
		return 0;

	return ( timeCur.GetTime() - timeLast.GetTime() ) / 3600 * 24;
}

int CSoftLastUseTask::UpdateCount( LPCTSTR pszFile, LPCTSTR pszIni, CTime& tLastUse )
{
	int nCnt = GetPrivateProfileInt( pszFile, L"count", -1, pszIni );
	CTime timeCur = CTime::GetCurrentTime();

	//没有记录过count时
	if( nCnt == -1 )
	{
		int nDayCnt = GetDayOver( timeCur, tLastUse );
		if( nDayCnt > 15 )
		{
			nCnt = 0;
		}
		else if( nDayCnt >= 4 )
		{
			nCnt = 10;
		}
		else
		{
			nCnt = 22;
		}

		CString tm, strCnt;				
		strCnt.Format( L"%d", nCnt );
		tm.Format(L"%I64d",timeCur.GetTime() - 3600 * 24 );
		WritePrivateProfileString( pszFile, L"lastuse", tm,		pszIni );
		WritePrivateProfileString( pszFile, L"count", strCnt,	pszIni );
	}
	else 
	{
		//过一个月次数减半,  有count时
		if( timeCur.GetMonth() != tLastUse.GetMonth() )
		{
			nCnt = nCnt / 2;
			CString tm, strCnt;				
			strCnt.Format( L"%d", nCnt );
			tm.Format(L"%I64d",timeCur.GetTime() );
			WritePrivateProfileString( pszFile, L"lastuse", tm,		pszIni );
			WritePrivateProfileString( pszFile, L"count", strCnt,	pszIni );
		}
	}
	
	return nCnt;
}

BOOL CSoftLastUseTask::TaskProcess(conew::CTaskMgr *pMgr)
{
	ISQLiteComDatabase3 *pDB = _pSoftUninst->GetDBPtr();
	wstring logPath = _pSoftUninst->GetKSafePath() + SOFT_STARTUP_LOG;

	//
	// 分析记录文件
	//
	WStrList dirList;
	{
		DWORD size = 8192;
		auto_buffer<wchar_t> buffer(size);

		do
		{
			DWORD ret = ::GetPrivateProfileSectionNamesW(&buffer[0], size, logPath.c_str());
			if(ret == 0)
			{
				break;
			}
			else if(ret < size - 2)
			{
				LPWSTR pStr = &buffer[0];
				while(*pStr != L'\0')
				{
					dirList.push_back(pStr);
					make_lower(dirList.back());

					pStr += dirList.back().size() + 1;
				}

				break;
			}

			size *= 2;
			if(!buffer.resize(size)) return FALSE;
		}
		while(TRUE);

		if(dirList.empty()) return FALSE;

		dirList.erase(unordered_unique(dirList.begin(), dirList.end()), dirList.end());
	}
	if(pMgr->IsExited()) return FALSE;

	//
	// 更新软件最后使用时间
	//
	SoftData2List softData2List;
	{
		pDB->BeginTransaction();
		{
			WStrHash keyHash;
			CCacheLastUse lastUse(pDB);
			CCacheLastUse updateCnt( pDB );
			CCacheSoftFind finder(pDB);

			WStrListIter end = dirList.end();
			for(WStrListIter it = dirList.begin(); it != end; ++it)
			{
				TCHAR tszUseTime[20] = {0};
				GetPrivateProfileString(it->c_str(), L"lastuse", TEXT("0"), tszUseTime, 20, logPath.c_str());

				if( _tcsicmp( tszUseTime, TEXT("")) == 0 )
					continue;

				CTime tLastUse( _wtoi64_l( tszUseTime, 0) );

				int nCnt = UpdateCount(it->c_str(), logPath.c_str() ,tLastUse );

				wstring key;
				if(finder.Query(*it, key))
				{
					if(keyHash.find(key) == keyHash.end())	// 过滤重复软件
					{
						softData2List.push_back(SoftData2());
						softData2List.back()._key = key;
						softData2List.back()._mask = SDM_Key | SDM_LastUse | SDM_Count;
						softData2List.back()._count = nCnt;
						softData2List.back()._lastUse = tLastUse.GetTime();

						keyHash.insert(key);
					}
					else
					{
						SoftData2Iter _it = softData2List.begin();
						SoftData2Iter end = softData2List.end();
						for(; _it != end; ++_it)
						{
							if (_it->_key == key && _it->_lastUse < tLastUse.GetTime() )
							{
								_it->_lastUse = tLastUse.GetTime();
							}
						}
					}

					lastUse.Update(key, (LONG)tLastUse.GetTime() );
					updateCnt.UpdateCount(key, nCnt );
				}
				//////////////////////////////////////////////////////////////////////////
				//if(
				//	finder.Query(*it, key) &&
				//	keyHash.find(key) == keyHash.end()	// 过滤重复软件
				//	)
				//{
				//	softData2List.push_back(SoftData2());
				//	softData2List.back()._key = key;
				//	softData2List.back()._mask = SDM_Key | SDM_LastUse;
				//	softData2List.back()._lastUse = static_cast<LONG>(time);

				//	lastUse.Update(key, static_cast<LONG>(time));

				//	keyHash.insert(key);
				//}
			}
		}
		pDB->CommitTransaction();
		if(pMgr->IsExited()) return FALSE;

		//
		// 将软件安装目录的最后使用时间作为最后使用时间
		//
		pDB->BeginTransaction();
		{
			CComPtr<ISQLiteComResultSet3> pResult;
			pDB->ExecuteQuery(L"select soft_key, main_path from local_soft_list where main_path not null and last_use=0 ;", &pResult);
			if(pResult != NULL)
			{
				CCacheLastUse lastUse(pDB);

				while(!pResult->IsEof())
				{
					LPCWSTR pMainPath = pResult->GetAsString(L"main_path");

					WIN32_FILE_ATTRIBUTE_DATA wfad;
					if(::GetFileAttributesExW(pMainPath, GetFileExInfoStandard, &wfad))
					{
						__time32_t time;
						FileTimeToUnixTime(&wfad.ftLastWriteTime, &time);

						softData2List.push_back(SoftData2());

						SoftData2 &softData2 = softData2List.back();
						softData2._key = pResult->GetAsString(L"soft_key");
						softData2._mask = SDM_Key | SDM_LastUse | SDM_Count;
						softData2._count = -1;
						softData2._lastUse = static_cast<LONG>(time);

						lastUse.Update(softData2._key, time );
					}

					pResult->NextRow();
				}
			}
		}
		pDB->CommitTransaction();
		if(pMgr->IsExited()) return FALSE;
	}

	//
	// 通知更新
	//
	if(!softData2List.empty())
	{
		CSoftDataEnum softDataEnum(softData2List);
		_pSoftUninst->GetNotify()->SoftDataEvent(UE_Update, &softDataEnum);
	}

	return FALSE;
}

inline void FileTimeToUnixTime(LPFILETIME pft, __time32_t* pt)
{
	LONGLONG ll; // 64 bit value
	ll = (((LONGLONG)(pft->dwHighDateTime)) << 32) + pft->dwLowDateTime;
	*pt = (__time32_t)((ll - 116444736000000000ui64)/10000000ui64);
}

}