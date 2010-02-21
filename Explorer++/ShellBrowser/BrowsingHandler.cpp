/******************************************************************
 *
 * Project: ShellBrowser
 * File: BrowsingHandler.cpp
 * License: GPL - See COPYING in the top level directory
 *
 * Handles the browsing of directories.
 *
 * Written by David Erceg
 * www.explorerplusplus.com
 *
 *****************************************************************/

#include "stdafx.h"
#include <list>
#include "IShellView.h"
#include "iShellBrowser_internal.h"
#include "../Helper/Controls.h"
#include "../Helper/Helper.h"
#include "../Helper/FileOperations.h"
#include "../Helper/FolderSize.h"
#include "resource.h"


HRESULT CFolderView::BrowseFolder(TCHAR *szPath,UINT wFlags)
{
	LPITEMIDLIST pidlDirectory	= NULL;
	HRESULT hr;

	hr = GetIdlFromParsingName(szPath,&pidlDirectory);

	if(SUCCEEDED(hr))
	{
		hr = BrowseFolder(pidlDirectory,wFlags);

		CoTaskMemFree(pidlDirectory);
	}

	return hr;
}

HRESULT CFolderView::BrowseFolder(LPITEMIDLIST pidlDirectory,UINT wFlags)
{
	LPITEMIDLIST	pidl = NULL;
	TCHAR			szParsingPath[MAX_PATH];
	BOOL			StoreHistory = TRUE;
	HRESULT			hr;
	int				nItems = 0;

	SetCursor(LoadCursor(NULL,IDC_WAIT));

	pidl = ILClone(pidlDirectory);

	if(m_bFolderVisited)
	{
		SaveColumnWidths();
	}

	/* The path may not be absolute, in which case it will
	need to be completed. */
	hr = ParsePath(&pidl,wFlags,&StoreHistory);

	if(hr != S_OK)
	{
		/* Parsing error. */
		SetCursor(LoadCursor(NULL,IDC_ARROW));
		return E_FAIL;
	}

	m_bBrowsing = TRUE;
	EmptyIconFinderQueue();
	EmptyThumbnailsQueue();
	EmptyColumnQueue();
	EmptyFolderQueue();
	m_bBrowsing = FALSE;

	/* TODO: These should be wrapped in a
	critical section. Erase been called at
	the same time as clear will crash the
	program. */
	m_FilesAdded.clear();
	m_pFileSelectionList.clear();

	GetDisplayName(pidl,szParsingPath,SHGDN_FORPARSING);

	SendMessage(m_hOwner,WM_USER_STARTEDBROWSING,m_ID,(WPARAM)szParsingPath);

	//CheckFolderLockState(szParsingPath);

	StringCchCopy(m_CurDir,SIZEOF_ARRAY(m_CurDir),szParsingPath);

	if(StoreHistory)
		m_pPathManager->StoreIdl(pidl);

	if(m_bFolderVisited)
	{
		ResetFolderMemoryAllocations();
	}

	m_nTotalItems = 0;

	nItems = BrowseVirtualFolder(pidl);
	//EnumFolderContents(szParsingPath);

	CoTaskMemFree(pidl);

	/* Stop the list view from redrawing itself each time is inserted.
	Redrawing will be allowed once all items have being inserted.
	(reduces lag when a large number of items are going to be inserted). */
	SendMessage(m_hListView,WM_SETREDRAW,(WPARAM)FALSE,(LPARAM)NULL);

	ListView_DeleteAllItems(m_hListView);

	/* Window updates needs these to be set. */
	m_NumFilesSelected		= 0;
	m_NumFoldersSelected	= 0;

	m_ulTotalDirSize.QuadPart = 0;
	m_ulFileSelectionSize.QuadPart = 0;

	SetActiveColumnSet();
	SetCurrentViewModeInternal(m_ViewMode);

	InsertAwaitingItems(FALSE);

	VerifySortMode();
	SortFolder(m_SortMode);

	ListView_EnsureVisible(m_hListView,0,FALSE);

	/* Allow the listview to redraw itself once again. */
	SendMessage(m_hListView,WM_SETREDRAW,(WPARAM)TRUE,(LPARAM)NULL);

	m_bFolderVisited = TRUE;

	SetCursor(LoadCursor(NULL,IDC_ARROW));

	m_iUniqueFolderIndex++;

	return S_OK;
}

void inline CFolderView::InsertAwaitingItems(BOOL bInsertIntoGroup)
{
	LVITEM							lv;
	ULARGE_INTEGER					ulFileSize;
	list<AwaitingAdd_t>::iterator	itr;
	BOOL							bFileFiltered;
	unsigned int					nPrevItems;
	int								nAdded = 0;
	int								iItemIndex;

	nPrevItems = ListView_GetItemCount(m_hListView);

	m_nAwaitingAdd = (int)m_AwaitingAddList.size();

	if((nPrevItems + m_nAwaitingAdd) == 0)
	{
		if(m_bApplyFilter)
			SendMessage(m_hOwner,WM_USER_FILTERINGAPPLIED,m_ID,TRUE);
		else
			SendMessage(m_hOwner,WM_USER_FOLDEREMPTY,m_ID,TRUE);

		m_nTotalItems = 0;

		return;
	}
	else if(!m_bApplyFilter)
	{
		SendMessage(m_hOwner,WM_USER_FOLDEREMPTY,m_ID,FALSE);
	}

	/* Make the listview allocate space (for internal data strctures)
	for all the items at once, rather than individually.
	Acts as a speed optimization. */
	ListView_SetItemCount(m_hListView,m_nAwaitingAdd + nPrevItems);

	lv.mask			= LVIF_TEXT|LVIF_IMAGE|LVIF_PARAM;

	if(bInsertIntoGroup)
		lv.mask		|= LVIF_GROUPID;

	/* Constant for each item. */
	lv.iSubItem		= 0;

	if(m_bAutoArrange)
		ListView_SetAutoArrange(m_hListView,FALSE);

	for(itr = m_AwaitingAddList.begin();itr != m_AwaitingAddList.end();itr++)
	{
		BOOL bHideFile;
		BOOL bHideSystemFile;

		if(m_bApplyFilter)
		{
			bFileFiltered = IsFileFiltered(m_pExtraItemInfo[itr->iItemInternal].szDisplayName)
				&& ((m_pwfdFiles[itr->iItemInternal].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != FILE_ATTRIBUTE_DIRECTORY);
		}
		else
		{
			bFileFiltered = FALSE;
		}

		if(m_bHideSystemFiles)
		{
			bHideSystemFile = (m_pwfdFiles[itr->iItemInternal].dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)
				== FILE_ATTRIBUTE_SYSTEM;
		}
		else
		{
			bHideSystemFile = FALSE;
		}

		bHideFile = bFileFiltered || bHideSystemFile;

		if(!bHideFile)
		{
			lv.iItem	= itr->iItem;

			BOOL bHideExtension = FALSE;
			TCHAR *pExt = NULL;

			if(m_bHideLinkExtension)
			{
				pExt = PathFindExtension(m_pExtraItemInfo[(int)itr->iItemInternal].szDisplayName);

				if(*pExt != '\0')
				{
					if(lstrcmpi(pExt,_T(".lnk")) == 0)
						bHideExtension = TRUE;
				}
			}

			/* Strip the extension if necessary. Don't remove the extension
			if the filename starts with a dot. */
			if((m_bShowExtensions ||
				m_pExtraItemInfo[(int)itr->iItemInternal].szDisplayName[0] == '.') &&
				!bHideExtension)
			{
				lv.pszText	= m_pExtraItemInfo[(int)itr->iItemInternal].szDisplayName;
			}
			else
			{
				TCHAR szDisplayName[MAX_PATH];

				StringCchCopy(szDisplayName,SIZEOF_ARRAY(szDisplayName),
				m_pExtraItemInfo[(int)itr->iItemInternal].szDisplayName);

				/* Strip the extension. */
				PathRemoveExtension(szDisplayName);

				/* The item will now be shown without its extension. */
				lv.pszText	= szDisplayName;
			}

			lv.iImage	= I_IMAGECALLBACK;
			lv.lParam	= itr->iItemInternal;

			if(bInsertIntoGroup)
			{
				lv.iGroupId	= DetermineItemGroup(itr->iItemInternal);
			}

			/* Insert the item into the list view control. */
			iItemIndex = ListView_InsertItem(m_hListView,&lv);

			if(itr->bPosition && m_ViewMode != VM_DETAILS)
			{
				POINT ptItem;

				if(itr->iAfter != -1)
				{
					ListView_GetItemPosition(m_hListView,itr->iAfter,&ptItem);
				}
				else
				{
					ptItem.x = 0;
					ptItem.y = 0;
				}

				/* The item will end up in the position AFTER iAfter. */
				ListView_SetItemPosition32(m_hListView,iItemIndex,ptItem.x,ptItem.y);
			}

			/* TODO: Fix. */
			if(m_ViewMode == VM_TILES)
			{
				SetTileViewItemInfo(iItemIndex,itr->iItemInternal);
			}


			if(m_bNewItemCreated)
			{
				LPITEMIDLIST pidlComplete = NULL;

				pidlComplete = ILCombine(m_pidlDirectory,m_pExtraItemInfo[(int)itr->iItemInternal].pridl);

				if(ILIsEqual(pidlComplete,m_pidlNewItem))
					m_bNewItemCreated = FALSE;

				m_iIndexNewItem = iItemIndex;

				CoTaskMemFree(pidlComplete);
			}


			/* If the file is marked as hidden, ghost it out. */
			if(m_pwfdFiles[itr->iItemInternal].dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)
			{
				ListView_SetItemState(m_hListView,iItemIndex,LVIS_CUT,LVIS_CUT);
			}
			
			/* Add the current file's size to the running size of the current directory. */
			/* A folder may or may not have 0 in its high file size member.
			It should either be zeroed, or never counted. */
			ulFileSize.LowPart = m_pwfdFiles[itr->iItemInternal].nFileSizeLow;
			ulFileSize.HighPart = m_pwfdFiles[itr->iItemInternal].nFileSizeHigh;

			m_ulTotalDirSize.QuadPart += ulFileSize.QuadPart;

			nAdded++;
		}
		else
		{
			m_FilteredItemsList.push_back(itr->iItemInternal);
		}
	}

	if(m_bAutoArrange)
		ListView_SetAutoArrange(m_hListView,TRUE);

	m_nTotalItems = nPrevItems + nAdded;

	if(m_ViewMode == VM_DETAILS)
	{
		QueueUserAPC(SetAllColumnDataAPC,m_hThread,(ULONG_PTR)this);

		if(m_bShowFolderSizes)
			QueueUserAPC(SetAllFolderSizeColumnDataAPC,m_hFolderSizeThread,(ULONG_PTR)this);
	}

	PositionDroppedItems();

	m_AwaitingAddList.clear();
	m_nAwaitingAdd = 0;
}

void CFolderView::RemoveItem(int iItemInternal)
{
	ULARGE_INTEGER	ulFileSize;
	LVFINDINFO		lvfi;
	BOOL			bFolder;
	int				iItem;
	int				nItems;

	if(iItemInternal == -1)
		return;

	CoTaskMemFree(m_pExtraItemInfo[iItemInternal].pridl);

	/* Is this item a folder? */
	bFolder = (m_pwfdFiles[iItemInternal].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ==
	FILE_ATTRIBUTE_DIRECTORY;

	/* Take the file size of the removed file away from the total
	directory size. */
	ulFileSize.LowPart = m_pwfdFiles[iItemInternal].nFileSizeLow;
	ulFileSize.HighPart = m_pwfdFiles[iItemInternal].nFileSizeHigh;

	m_ulTotalDirSize.QuadPart -= ulFileSize.QuadPart;

	/* Locate the item within the listview.
	Could use filename, providing removed
	items are always deleted before new
	items are inserted. */
	lvfi.flags	= LVFI_PARAM;
	lvfi.lParam	= iItemInternal;
	iItem = ListView_FindItem(m_hListView,-1,&lvfi);
	
	if(iItem != -1)
	{
		/* Remove the item from the listview. */
		ListView_DeleteItem(m_hListView,iItem);
	}

	/* Invalidate the items internal data.
	This will mark it as free, so that it
	can be used by another item. */
	m_pItemMap[iItemInternal] = 0;

	nItems = ListView_GetItemCount(m_hListView);

	m_nTotalItems--;

	if(nItems == 0 && !m_bApplyFilter)
	{
		SendMessage(m_hOwner,WM_USER_FOLDEREMPTY,m_ID,TRUE);
	}
}

HRESULT CFolderView::ParsePath(LPITEMIDLIST *pidlDirectory,UINT uFlags,
BOOL *bStoreHistory)
{
	if((uFlags & SBSP_RELATIVE) == SBSP_RELATIVE)
	{
		LPITEMIDLIST	pidlComplete;

		if(pidlDirectory == NULL)
			return E_INVALIDARG;

		/* This is a relative path. Add it on to the end of the current directory
		name to get a fully qualified path. */
		pidlComplete = ILCombine(m_pidlDirectory,*pidlDirectory);

		*pidlDirectory = ILClone(pidlComplete);

		CoTaskMemFree(pidlComplete);
	}
	else if((uFlags & SBSP_PARENT) == SBSP_PARENT)
	{
		HRESULT hr;

		hr = GetVirtualParentPath(m_pidlDirectory,pidlDirectory);
	}
	else if((uFlags & SBSP_NAVIGATEBACK) == SBSP_NAVIGATEBACK)
	{
		if(m_pPathManager->GetNumBackPathsStored() == 0)
		{
			SetFocus(m_hListView);
			return E_FAIL;
		}

		/*Gets the path of the folder that was last visited.
		Ignores the supplied Path argument.*/
		*bStoreHistory		= FALSE;

		*pidlDirectory = m_pPathManager->RetrieveAndValidateIdl(-1);
	}
	else if((uFlags & SBSP_NAVIGATEFORWARD) == SBSP_NAVIGATEFORWARD)
	{
		if(m_pPathManager->GetNumForwardPathsStored() == 0)
		{
			SetFocus(m_hListView);
			return E_FAIL;
		}

		/*Gets the path of the folder that is 'forward' of
		this one. Ignores the supplied Path argument.*/
		*bStoreHistory		= FALSE;

		*pidlDirectory = m_pPathManager->RetrieveAndValidateIdl(1);
	}
	else
	{
		/* Assume that SBSP_ABSOLUTE was paseed. */
		if(pidlDirectory == NULL)
			return E_INVALIDARG;
	}
	
	if((uFlags & SBSP_WRITENOHISTORY) == SBSP_WRITENOHISTORY)
	{
		/* Client has requested that the folder to be browsed to will have
		no history item associated with it. */
		*bStoreHistory		= FALSE;
	}

	if(!CheckIdl(*pidlDirectory))
		return E_FAIL;

	return S_OK;
}

int CFolderView::BrowseVirtualFolder(TCHAR *szParsingName)
{
	LPITEMIDLIST pidl				= NULL;
	int nItems = 0;

	GetIdlFromParsingName(szParsingName,&pidl);

	nItems = BrowseVirtualFolder(pidl);

	CoTaskMemFree(pidl);

	return nItems;
}

int CFolderView::BrowseVirtualFolder(LPITEMIDLIST pidlDirectory)
{
	IShellFolder	*pDesktopFolder	= NULL;
	IShellFolder	*pShellFolder		= NULL;
	IEnumIDList		*pEnumIDList		= NULL;
	LPITEMIDLIST	rgelt				= NULL;
	STRRET			str;
	SHCONTF			EnumFlags;
	TCHAR			szFileName[MAX_PATH];
	ULONG			uFetched;
	HRESULT			hr;
	int				nItems = 0;

	DetermineFolderVirtual(pidlDirectory);

	hr = SHGetDesktopFolder(&pDesktopFolder);

	if(SUCCEEDED(hr))
	{
		if(IsNamespaceRoot(pidlDirectory))
		{
			hr = SHGetDesktopFolder(&pShellFolder);
			m_bVirtualFolder = TRUE;
		}
		else
		{
			hr = pDesktopFolder->BindToObject(pidlDirectory,NULL,
			IID_IShellFolder,(LPVOID *)&pShellFolder);
		}

		m_pidlDirectory = ILClone(pidlDirectory);

		if(SUCCEEDED(hr))
		{
			EnumFlags = SHCONTF_FOLDERS|SHCONTF_NONFOLDERS;

			if(m_bShowHidden)
				EnumFlags |= SHCONTF_INCLUDEHIDDEN;

			hr = pShellFolder->EnumObjects(m_hOwner,EnumFlags,&pEnumIDList);

			if(SUCCEEDED(hr) && pEnumIDList != NULL)
			{
				uFetched = 1;
				while(pEnumIDList->Next(1,&rgelt,&uFetched) == S_OK && (uFetched == 1))
				{
					/* If this is a virtual folder, only use SHGDN_INFOLDER. If this is
					a real folder, combine SHGDN_INFOLDER with SHGDN_FORPARSING. This is
					so that items in real folders can still be shown with extensions, even
					if the global, Explorer option is disabled. */
					if(m_bVirtualFolder)
						hr = pShellFolder->GetDisplayNameOf(rgelt,SHGDN_INFOLDER,&str);
					else
						hr = pShellFolder->GetDisplayNameOf(rgelt,SHGDN_INFOLDER|SHGDN_FORPARSING,&str);

					if(SUCCEEDED(hr))
					{
						StrRetToBuf(&str,rgelt,szFileName,MAX_PATH);

						AddItemInternal(pidlDirectory,rgelt,szFileName,-1,FALSE);

						nItems++;
					}

					CoTaskMemFree((LPVOID)rgelt);
				}

				pEnumIDList->Release();
			}

			pShellFolder->Release();
		}

		pDesktopFolder->Release();
	}

	return nItems;
}

HRESULT CFolderView::EnumFolderContents(TCHAR *szFolderPath)
{
	HANDLE hFirstFile			= NULL;
	IShellFolder *pShellFolder	= NULL;
	LPITEMIDLIST pidlComplete	= NULL;
	LPITEMIDLIST pidlRelative	= NULL;
	AwaitingAdd_t AwaitingAdd;
	TCHAR szFindPath[MAX_PATH + 3];
	TCHAR szFullFileName[MAX_PATH];
	int uItemId;

	if(szFolderPath)
		return E_INVALIDARG;

	GetIdlFromParsingName(szFolderPath,&m_pidlDirectory);

	StringCchCopy(szFindPath,MAX_PATH,szFolderPath);
	
	/* These extra characters are needed for the file search to work properly. */
	PathAppend(szFindPath,_T("*"));

	hFirstFile = FindFirstFile(szFindPath,&m_pwfdFiles[0]);

	/* No files in the directory. Return without doing anything further. */
	if(hFirstFile == INVALID_HANDLE_VALUE)
		return S_OK;

	uItemId = GenerateUniqueItemId();

	while(FindNextFile(hFirstFile,&m_pwfdFiles[uItemId]) != 0)
	{
		if(lstrcmp(m_pwfdFiles[uItemId].cFileName,_T("..")) != 0)
		{
			if((m_nTotalItems + m_nAwaitingAdd) > (m_iCurrentAllocation - 1))
			{
				int PrevSize = m_iCurrentAllocation;

				if(m_iCurrentAllocation > MEM_ALLOCATION_LEVEL_MEDIUM)
					m_iCurrentAllocation += MEM_ALLOCATION_LEVEL_MEDIUM;
				else if(m_iCurrentAllocation > MEM_ALLOCATION_LEVEL_LOW)
					m_iCurrentAllocation += MEM_ALLOCATION_LEVEL_LOW;
				else
					m_iCurrentAllocation += DEFAULT_MEM_ALLOC;

				m_pwfdFiles = (WIN32_FIND_DATA *)realloc(m_pwfdFiles,
					m_iCurrentAllocation * (sizeof(WIN32_FIND_DATA)));

				m_pExtraItemInfo = (CItemObject *)realloc(m_pExtraItemInfo,
					m_iCurrentAllocation * sizeof(CItemObject));

				m_pItemMap = (int *)realloc(m_pItemMap,m_iCurrentAllocation * sizeof(int));

				InitializeItemMap(PrevSize,m_iCurrentAllocation);

				if(m_pwfdFiles == NULL || m_pExtraItemInfo == NULL)
				{
					return E_OUTOFMEMORY;
				}
			}

			StringCchCopy(szFullFileName,MAX_PATH,szFolderPath);
			PathAppend(szFullFileName,m_pwfdFiles[uItemId].cFileName);
			GetIdlFromParsingName(szFullFileName,&pidlComplete);
			SHBindToParent(pidlComplete,IID_IShellFolder,(void **)&pShellFolder,(LPCITEMIDLIST *)&pidlRelative);
			CoTaskMemFree(pidlComplete);

			m_pExtraItemInfo[uItemId].pridl						= ILClone(pidlRelative);
			m_pExtraItemInfo[uItemId].bReal						= TRUE;
			StringCchCopy(m_pExtraItemInfo[uItemId].szDisplayName,MAX_PATH,m_pwfdFiles[uItemId].cFileName);

			AwaitingAdd.iItem			= m_nTotalItems + m_nAwaitingAdd;
			AwaitingAdd.iItemInternal	= uItemId;

			m_AwaitingAddList.push_back(AwaitingAdd);
			m_nAwaitingAdd++;
		}
	}

	FindClose(hFirstFile);

	return S_OK;
}

HRESULT inline CFolderView::AddItemInternal(LPITEMIDLIST pidlDirectory,
LPITEMIDLIST pidlRelative,TCHAR *szFileName,int iItemIndex,BOOL bPosition)
{
	int				uItemId;

	uItemId = SetItemInformation(pidlDirectory,pidlRelative,szFileName);

	return AddItemInternal(iItemIndex,uItemId,bPosition);
}

HRESULT inline CFolderView::AddItemInternal(int iItemIndex,int iItemId,BOOL bPosition)
{
	AwaitingAdd_t	AwaitingAdd;

	if(iItemIndex == -1)
		AwaitingAdd.iItem = m_nTotalItems + m_nAwaitingAdd - 1;
	else
		AwaitingAdd.iItem = iItemIndex;

	AwaitingAdd.iItemInternal = iItemId;
	AwaitingAdd.bPosition = bPosition;
	AwaitingAdd.iAfter = iItemIndex - 1;

	m_AwaitingAddList.push_back(AwaitingAdd);

	AddToColumnQueue(AwaitingAdd.iItem);
	AddToFolderQueue(AwaitingAdd.iItem);

	return S_OK;
}

int inline CFolderView::SetItemInformation(LPITEMIDLIST pidlDirectory,
LPITEMIDLIST pidlRelative,TCHAR *szFileName)
{
	LPITEMIDLIST	pidlItem = NULL;
	HANDLE			hFirstFile;
	TCHAR			szPath[MAX_PATH];
	int				uItemId;

	m_nAwaitingAdd++;

	if((m_nTotalItems + m_nAwaitingAdd) > (m_iCurrentAllocation - 1))
	{
		int PrevSize = m_iCurrentAllocation;

		if(m_iCurrentAllocation > MEM_ALLOCATION_LEVEL_MEDIUM)
			m_iCurrentAllocation += MEM_ALLOCATION_LEVEL_MEDIUM;
		else if(m_iCurrentAllocation > MEM_ALLOCATION_LEVEL_LOW)
			m_iCurrentAllocation += MEM_ALLOCATION_LEVEL_LOW;
		else
			m_iCurrentAllocation += DEFAULT_MEM_ALLOC;

		m_pwfdFiles = (WIN32_FIND_DATA *)realloc(m_pwfdFiles,
			m_iCurrentAllocation * (sizeof(WIN32_FIND_DATA)));

		m_pExtraItemInfo = (CItemObject *)realloc(m_pExtraItemInfo,
			m_iCurrentAllocation * sizeof(CItemObject));

		m_pItemMap = (int *)realloc(m_pItemMap,m_iCurrentAllocation * sizeof(int));

		InitializeItemMap(PrevSize,m_iCurrentAllocation);

		if(m_pwfdFiles == NULL || m_pExtraItemInfo == NULL)
			return E_OUTOFMEMORY;
	}

	m_nInfoListAllocation = m_nTotalItems + m_nAwaitingAdd;

	uItemId = GenerateUniqueItemId();

	m_pExtraItemInfo[uItemId].pridl					= ILClone(pidlRelative);
	m_pExtraItemInfo[uItemId].bIconRetrieved		= FALSE;
	m_pExtraItemInfo[uItemId].bThumbnailRetreived	= FALSE;
	m_pExtraItemInfo[uItemId].bFolderSizeRetrieved	= FALSE;
	StringCchCopy(m_pExtraItemInfo[uItemId].szDisplayName,MAX_PATH,szFileName);

	pidlItem = ILCombine(pidlDirectory,pidlRelative);

	SHGetPathFromIDList(pidlItem,szPath);

	CoTaskMemFree(pidlItem);

	/* DO NOT call FindFirstFile() on root drives (especially
	floppy drives). Doing so may cause a delay of up to a
	few seconds. */
	if(!PathIsRoot(szPath))
	{
		m_pExtraItemInfo[uItemId].bDrive = FALSE;
		hFirstFile = FindFirstFile(szPath,&m_pwfdFiles[uItemId]);
	}
	else
	{
		m_pExtraItemInfo[uItemId].bDrive = TRUE;
		StringCchCopy(m_pExtraItemInfo[uItemId].szDrive,
			SIZEOF_ARRAY(m_pExtraItemInfo[uItemId].szDrive),
			szPath);

		hFirstFile = INVALID_HANDLE_VALUE;
	}

	/* Need to use this, since may be in a virtual folder
	(such as the recycle bin), but items still exist. */
	if(hFirstFile != INVALID_HANDLE_VALUE)
	{
		m_pExtraItemInfo[uItemId].bReal = TRUE;
		FindClose(hFirstFile);
	}
	else
	{
		StringCchCopy(m_pwfdFiles[uItemId].cFileName,
			sizeof(m_pwfdFiles[uItemId].cFileName)/sizeof(m_pwfdFiles[uItemId].cFileName[0]),szFileName);
		m_pwfdFiles[uItemId].nFileSizeLow			= 0;
		m_pwfdFiles[uItemId].nFileSizeHigh			= 0;
		m_pwfdFiles[uItemId].dwFileAttributes		= FILE_ATTRIBUTE_DIRECTORY;

		m_pExtraItemInfo[uItemId].bReal = FALSE;
	}

	return uItemId;
}