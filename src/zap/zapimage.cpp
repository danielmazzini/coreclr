//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.
//
//
// ZapImage.cpp
//

//
// NGEN-specific infrastructure for writing PE files.
// 
// ======================================================================================

#include "common.h"
#include "strsafe.h"

#include "zaprelocs.h"

#include "zapinnerptr.h"
#include "zapwrapper.h"

#include "zapheaders.h"
#include "zapmetadata.h"
#include "zapcode.h"
#include "zapimport.h"

#ifdef FEATURE_READYTORUN_COMPILER
#include "zapreadytorun.h"
#endif

#include "md5.h"

#ifdef  MDIL
#include "WellKnownTypes.h"
struct GuidInfo;
class MethodDesc;
class MethodTable;
#include "CompactLayoutWriter.h"
#endif

// This is RTL_CONTAINS_FIELD from ntdef.h
#define CONTAINS_FIELD(Struct, Size, Field) \
    ( (((PCHAR)(&(Struct)->Field)) + sizeof((Struct)->Field)) <= (((PCHAR)(Struct))+(Size)) )

/* --------------------------------------------------------------------------- *
 * Destructor wrapper objects
 * --------------------------------------------------------------------------- */

ZapImage::ZapImage(Zapper *zapper)
  : m_zapper(zapper)
    /* Everything else is initialized to 0 by default */
{
#ifndef FEATURE_CORECLR
    if (m_zapper->m_pOpt->m_statOptions)
        m_stats = new ZapperStats();
#endif
}

ZapImage::~ZapImage()
{
#ifdef ZAP_HASHTABLE_TUNING
    // If ZAP_HASHTABLE_TUNING is defined, preallocate is overloaded to print the tunning constants
    Preallocate();
#endif

    //
    // Clean up.
    //
#ifndef FEATURE_CORECLR
    if (m_stats != NULL)
        delete m_stats;
#endif

    if (m_pModuleFileName != NULL)
        delete [] m_pModuleFileName;

    if (m_pMDImport != NULL)
        m_pMDImport->Release();

    if (m_pAssemblyEmit != NULL)
        m_pAssemblyEmit->Release();

    if (m_profileDataFile != NULL)
        UnmapViewOfFile(m_profileDataFile);

    if (m_pPreloader)
        m_pPreloader->Release();

    if (m_pImportSectionsTable != NULL)
        m_pImportSectionsTable->~ZapImportSectionsTable();

    if (m_pGCInfoTable != NULL)
        m_pGCInfoTable->~ZapGCInfoTable();

#ifdef WIN64EXCEPTIONS
    if (m_pUnwindDataTable != NULL)
        m_pUnwindDataTable->~ZapUnwindDataTable();
#endif

    if (m_pStubDispatchDataTable != NULL)
        m_pStubDispatchDataTable->~ZapImportSectionSignatures();

    if (m_pExternalMethodDataTable != NULL)
        m_pExternalMethodDataTable->~ZapImportSectionSignatures();

    if (m_pDynamicHelperDataTable != NULL)
        m_pDynamicHelperDataTable->~ZapImportSectionSignatures();

    if (m_pDebugInfoTable != NULL)
        m_pDebugInfoTable->~ZapDebugInfoTable();

#ifdef MDIL
    if (m_pMdilDebugInfoTable != NULL)
        m_pMdilDebugInfoTable->~MdilDebugInfoTable();
#endif

    if (m_pVirtualSectionsTable != NULL)
        m_pVirtualSectionsTable->~ZapVirtualSectionsTable();

    if (m_pILMetaData != NULL)
        m_pILMetaData->~ZapILMetaData();

    if (m_pBaseRelocs != NULL)
        m_pBaseRelocs->~ZapBaseRelocs();

    if (m_pAssemblyMetaData != NULL)
        m_pAssemblyMetaData->~ZapMetaData();

    //
    // Destruction of auxiliary tables in alphabetical order
    //

    if (m_pImportTable != NULL) 
        m_pImportTable->~ZapImportTable();

    if (m_pInnerPtrs != NULL) 
        m_pInnerPtrs->~ZapInnerPtrTable();

    if (m_pMethodEntryPoints != NULL)
        m_pMethodEntryPoints->~ZapMethodEntryPointTable();

    if (m_pWrappers != NULL) 
        m_pWrappers->~ZapWrapperTable();
}

void ZapImage::InitializeSections()
{
    AllocateVirtualSections();

    m_pCorHeader = new (GetHeap()) ZapCorHeader(this);
    m_pHeaderSection->Place(m_pCorHeader);

    SetDirectoryEntry(IMAGE_DIRECTORY_ENTRY_COMHEADER, m_pCorHeader);

    m_pNativeHeader = new (GetHeap()) ZapNativeHeader(this);
    m_pHeaderSection->Place(m_pNativeHeader);

    m_pCodeManagerEntry = new (GetHeap()) ZapCodeManagerEntry(this);
    m_pHeaderSection->Place(m_pCodeManagerEntry);

    m_pImportSectionsTable = new (GetHeap()) ZapImportSectionsTable(this);
    m_pImportTableSection->Place(m_pImportSectionsTable);

    m_pExternalMethodDataTable = new (GetHeap()) ZapImportSectionSignatures(this, m_pExternalMethodThunkSection, m_pGCSection);
    m_pExternalMethodDataSection->Place(m_pExternalMethodDataTable);

    m_pStubDispatchDataTable = new (GetHeap()) ZapImportSectionSignatures(this, m_pStubDispatchCellSection, m_pGCSection);
    m_pStubDispatchDataSection->Place(m_pStubDispatchDataTable);

    m_pImportTable = new (GetHeap()) ZapImportTable(this);
    m_pImportTableSection->Place(m_pImportTable);

    m_pGCInfoTable = new (GetHeap()) ZapGCInfoTable(this);
    m_pExceptionInfoLookupTable = new (GetHeap()) ZapExceptionInfoLookupTable(this);

#ifdef WIN64EXCEPTIONS
    m_pUnwindDataTable = new (GetHeap()) ZapUnwindDataTable(this);
#endif

    m_pEEInfoTable = ZapBlob::NewAlignedBlob(this, NULL, sizeof(CORCOMPILE_EE_INFO_TABLE), sizeof(TADDR));
    m_pEETableSection->Place(m_pEEInfoTable);

    //
    // Allocate Helper table, and fill it out
    //

    m_pHelperThunks = new (GetHeap()) ZapNode * [CORINFO_HELP_COUNT];

#ifdef MDIL
    if (m_zapper->m_fEmbedMDIL)
    {
        if (m_cbMdilPESectionData != NULL)
        {
            ZapBlob *mdilData = ZapBlob::NewAlignedBlob(this, m_pMdilPESectionData, m_cbMdilPESectionData, sizeof(TADDR));
            m_pMDILSection->Place(mdilData);
        }
        else
        {
            m_zapper->Error(W("Could not embed mdil data in ni image. MDIL data not present in IL file.\n"));
            IfFailThrow(E_INVALIDARG);
        }
    }
#endif // MDIL

#ifdef FEATURE_CORECLR
    if (!m_zapper->m_pOpt->m_fNoMetaData)
#endif
    {
        m_pILMetaData = new (GetHeap()) ZapILMetaData(this);
        m_pILMetaDataSection->Place(m_pILMetaData);
    }

    m_pDebugInfoTable = new (GetHeap()) ZapDebugInfoTable(this);
    m_pDebugSection->Place(m_pDebugInfoTable);

#ifdef MDIL
    m_pMdilDebugInfoTable = new (GetHeap()) MdilDebugInfoTable(this);
#endif

    m_pBaseRelocs = new (GetHeap()) ZapBaseRelocs(this);
    m_pBaseRelocsSection->Place(m_pBaseRelocs);

    SetDirectoryEntry(IMAGE_DIRECTORY_ENTRY_BASERELOC, m_pBaseRelocsSection);

    //
    // Initialization of auxiliary tables in alphabetical order
    //
    m_pInnerPtrs = new (GetHeap()) ZapInnerPtrTable(this);
    m_pMethodEntryPoints = new (GetHeap()) ZapMethodEntryPointTable(this);
    m_pWrappers = new (GetHeap()) ZapWrapperTable(this);

    // Place the virtual sections tables in debug section. It exists for diagnostic purposes
    // only and should not be touched under normal circumstances    
    m_pVirtualSectionsTable = new (GetHeap()) ZapVirtualSectionsTable(this);
    m_pDebugSection->Place(m_pVirtualSectionsTable);

#ifndef ZAP_HASHTABLE_TUNING
    Preallocate();
#endif
}

#ifdef FEATURE_READYTORUN_COMPILER
void ZapImage::InitializeSectionsForReadyToRun()
{
    AllocateVirtualSections();

    // Preload sections are not used for ready to run. Clear the pointers to them to catch accidental use.
    for (int i = 0; i < CORCOMPILE_SECTION_COUNT; i++)
        m_pPreloadSections[i] = NULL;

    m_pCorHeader = new (GetHeap()) ZapCorHeader(this);
    m_pHeaderSection->Place(m_pCorHeader);

    SetDirectoryEntry(IMAGE_DIRECTORY_ENTRY_COMHEADER, m_pCorHeader);

    m_pNativeHeader = new (GetHeap()) ZapReadyToRunHeader(this);
    m_pHeaderSection->Place(m_pNativeHeader);

    m_pImportSectionsTable = new (GetHeap()) ZapImportSectionsTable(this);
    m_pHeaderSection->Place(m_pImportSectionsTable);

    {
#ifdef FEATURE_CORECLR
#define COMPILER_NAME "CoreCLR"
#else
#define COMPILER_NAME "CLR"
#endif

        const char * pCompilerIdentifier = COMPILER_NAME " " FX_FILEVERSION_STR " " QUOTE_MACRO(__BUILDMACHINE__);
        ZapBlob * pCompilerIdentifierBlob = new (GetHeap()) ZapBlobPtr((PVOID)pCompilerIdentifier, strlen(pCompilerIdentifier) + 1);

        GetReadyToRunHeader()->RegisterSection(READYTORUN_SECTION_COMPILER_IDENTIFIER, pCompilerIdentifierBlob);
        m_pHeaderSection->Place(pCompilerIdentifierBlob);
    }

    m_pImportTable = new (GetHeap()) ZapImportTable(this);
    m_pImportTableSection->Place(m_pImportTable);

    for (int i=0; i<ZapImportSectionType_Total; i++)
    {
        ZapVirtualSection * pSection;
        if (i == ZapImportSectionType_Eager)
            pSection = m_pDelayLoadInfoDelayListSectionEager;
        else
        if (i < ZapImportSectionType_Cold)
            pSection = m_pDelayLoadInfoDelayListSectionHot;
        else
            pSection = m_pDelayLoadInfoDelayListSectionCold;

        m_pDelayLoadInfoDataTable[i] = new (GetHeap()) ZapImportSectionSignatures(this, m_pDelayLoadInfoTableSection[i]);
        pSection->Place(m_pDelayLoadInfoDataTable[i]);
    }

    m_pDynamicHelperDataTable = new (GetHeap()) ZapImportSectionSignatures(this, m_pDynamicHelperCellSection);
    m_pDynamicHelperDataSection->Place(m_pDynamicHelperDataTable);

    m_pExternalMethodDataTable = new (GetHeap()) ZapImportSectionSignatures(this, m_pExternalMethodCellSection, m_pGCSection);
    m_pExternalMethodDataSection->Place(m_pExternalMethodDataTable);

    m_pStubDispatchDataTable = new (GetHeap()) ZapImportSectionSignatures(this, m_pStubDispatchCellSection, m_pGCSection);
    m_pStubDispatchDataSection->Place(m_pStubDispatchDataTable);

    m_pGCInfoTable = new (GetHeap()) ZapGCInfoTable(this);

#ifdef WIN64EXCEPTIONS
    m_pUnwindDataTable = new (GetHeap()) ZapUnwindDataTable(this);
#endif

    m_pILMetaData = new (GetHeap()) ZapILMetaData(this);
    m_pILMetaDataSection->Place(m_pILMetaData);

    m_pBaseRelocs = new (GetHeap()) ZapBaseRelocs(this);
    m_pBaseRelocsSection->Place(m_pBaseRelocs);

    SetDirectoryEntry(IMAGE_DIRECTORY_ENTRY_BASERELOC, m_pBaseRelocsSection);

    //
    // Initialization of auxiliary tables in alphabetical order
    //
    m_pInnerPtrs = new (GetHeap()) ZapInnerPtrTable(this);

    m_pExceptionInfoLookupTable = new (GetHeap()) ZapExceptionInfoLookupTable(this);

    //
    // Always allocate slot for module - it is used to determine that the image is used
    //
    m_pImportTable->GetPlacedHelperImport(READYTORUN_HELPER_Module);
}
#endif // FEATURE_READYTORUN_COMPILER


#define DATA_MEM_READONLY IMAGE_SCN_MEM_READ
#define DATA_MEM_WRITABLE IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE
#define XDATA_MEM         IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE
#define TEXT_MEM          IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ

void ZapImage::AllocateVirtualSections()
{
    //
    // Allocate all virtual sections in the order they will appear in the final image
    //
    // To maximize packing of the data in the native image, the number of named physical sections is minimized -  
    // the named physical sections are used just for memory protection control. All items with the same memory
    // protection are packed together in one physical section.
    //

    {
        //
        // .data section
        //
        DWORD access = DATA_MEM_WRITABLE;

#ifdef FEATURE_LAZY_COW_PAGES
        // READYTORUN: FUTURE: Optional support for COW pages
        if (!IsReadyToRunCompilation() && CLRConfig::GetConfigValue(CLRConfig::INTERNAL_ZapLazyCOWPagesEnabled))
            access = DATA_MEM_READONLY;
#endif

        ZapPhysicalSection * pDataSection = NewPhysicalSection(".data", IMAGE_SCN_CNT_INITIALIZED_DATA | access);

        m_pPreloadSections[CORCOMPILE_SECTION_MODULE] = NewVirtualSection(pDataSection, IBCUnProfiledSection | HotRange | ModuleSection);

        m_pEETableSection = NewVirtualSection(pDataSection, IBCUnProfiledSection | HotRange | EETableSection); // Could be marked bss if it makes sense

        // These are all known to be hot or writeable
        m_pPreloadSections[CORCOMPILE_SECTION_WRITE] = NewVirtualSection(pDataSection, IBCProfiledSection | HotRange | WriteDataSection);
        m_pPreloadSections[CORCOMPILE_SECTION_HOT_WRITEABLE] = NewVirtualSection(pDataSection, IBCProfiledSection | HotRange | WriteableDataSection); // hot for reading, potentially written to 
        m_pPreloadSections[CORCOMPILE_SECTION_WRITEABLE] = NewVirtualSection(pDataSection, IBCProfiledSection | ColdRange | WriteableDataSection); // Cold based on IBC profiling data.
        m_pPreloadSections[CORCOMPILE_SECTION_HOT] = NewVirtualSection(pDataSection, IBCProfiledSection | HotRange | DataSection);

        m_pPreloadSections[CORCOMPILE_SECTION_RVA_STATICS_HOT] = NewVirtualSection(pDataSection, IBCProfiledSection | HotRange | RVAStaticsSection);

        m_pDelayLoadInfoTableSection[ZapImportSectionType_Eager] = NewVirtualSection(pDataSection, IBCUnProfiledSection | HotRange | DelayLoadInfoTableEagerSection, sizeof(TADDR));

        //
        // Allocate dynamic info tables
        //

        // Place the HOT CorCompileTables now, the cold ones would be placed later in this routine (after other HOT sections)
        for (int i=0; i<ZapImportSectionType_Count; i++)
        {
            m_pDelayLoadInfoTableSection[i] = NewVirtualSection(pDataSection, IBCProfiledSection | HotRange | DelayLoadInfoTableSection, sizeof(TADDR));
        }

        m_pDynamicHelperCellSection = NewVirtualSection(pDataSection, IBCProfiledSection | HotColdSortedRange | ExternalMethodDataSection, sizeof(TADDR));

        m_pExternalMethodCellSection = NewVirtualSection(pDataSection, IBCProfiledSection | HotColdSortedRange | ExternalMethodThunkSection, sizeof(TADDR));

        // m_pStubDispatchCellSection is  deliberately placed  directly after
        // the last m_pDelayLoadInfoTableSection (all .data sections go together in the order indicated).
        // We do this to place it as the last "hot, written" section.  Why? Because
        // we don't split the dispatch cells into hot/cold sections (We probably should),
        // and so the section is actually half hot and half cold.
        // But it turns out that the hot dispatch cells always come
        // first (because the code that uses them is hot and gets compiled first).
        // Thus m_pStubDispatchCellSection contains all hot cells at the front of
        // this blob of data.  By making them last in a grouping of written data we
        // make sure the hot data is grouped with hot data in the
        // m_pDelayLoadInfoTableSection sections.

        m_pStubDispatchCellSection = NewVirtualSection(pDataSection, IBCProfiledSection | HotColdSortedRange | StubDispatchDataSection, sizeof(TADDR));

        // Earlier we placed the HOT corCompile tables. Now place the cold ones after the stub dispatch cell section. 
        for (int i=0; i<ZapImportSectionType_Count; i++)
        {
            m_pDelayLoadInfoTableSection[ZapImportSectionType_Cold + i] = NewVirtualSection(pDataSection, IBCProfiledSection | ColdRange | DelayLoadInfoTableSection, sizeof(TADDR));
        }

        //
        // Virtual sections that are moved to .cdata when we have profile data.
        //

        // This is everyhing that is assumed to be warm in the first strata
        // of non-profiled scenarios.  MethodTables related to objects etc.
        m_pPreloadSections[CORCOMPILE_SECTION_WARM] = NewVirtualSection(pDataSection, IBCProfiledSection | WarmRange | EEDataSection, sizeof(TADDR));

        m_pPreloadSections[CORCOMPILE_SECTION_RVA_STATICS_COLD] = NewVirtualSection(pDataSection, IBCProfiledSection | ColdRange | RVAStaticsSection);

        // In an ideal world these are cold in both profiled and the first strata
        // of non-profiled scenarios (i.e. no reflection, etc. )  The sections at the
        // bottom correspond to further strata of non-profiled scenarios.
        m_pPreloadSections[CORCOMPILE_SECTION_CLASS_COLD] = NewVirtualSection(pDataSection, IBCProfiledSection | ColdRange | ClassSection, sizeof(TADDR));
        m_pPreloadSections[CORCOMPILE_SECTION_CROSS_DOMAIN_INFO] = NewVirtualSection(pDataSection, IBCUnProfiledSection | ColdRange | CrossDomainInfoSection, sizeof(TADDR));
        m_pPreloadSections[CORCOMPILE_SECTION_METHOD_DESC_COLD] = NewVirtualSection(pDataSection, IBCProfiledSection | ColdRange | MethodDescSection, sizeof(TADDR));
        m_pPreloadSections[CORCOMPILE_SECTION_METHOD_DESC_COLD_WRITEABLE] = NewVirtualSection(pDataSection, IBCProfiledSection | ColdRange | MethodDescWriteableSection, sizeof(TADDR));
        m_pPreloadSections[CORCOMPILE_SECTION_MODULE_COLD] = NewVirtualSection(pDataSection, IBCProfiledSection | ColdRange | ModuleSection, sizeof(TADDR));
        m_pPreloadSections[CORCOMPILE_SECTION_DEBUG_COLD] = NewVirtualSection(pDataSection, IBCUnProfiledSection | ColdRange | DebugSection, sizeof(TADDR));

        //
        // If we're instrumenting allocate a section for writing profile data
        //
        if (m_zapper->m_pOpt->m_compilerFlags & CORJIT_FLG_BBINSTR)
        {
            m_pInstrumentSection = NewVirtualSection(pDataSection, IBCUnProfiledSection | ColdRange | InstrumentSection, sizeof(TADDR));
        }
    }

    // No RWX pages in ready to run images
    if (!IsReadyToRunCompilation())
    {
        DWORD access = XDATA_MEM;

#ifdef FEATURE_LAZY_COW_PAGES
        if (CLRConfig::GetConfigValue(CLRConfig::INTERNAL_ZapLazyCOWPagesEnabled))
            access = TEXT_MEM;
#endif            

        //
        // .xdata section
        //
        ZapPhysicalSection * pXDataSection  = NewPhysicalSection(".xdata", IMAGE_SCN_CNT_INITIALIZED_DATA | access);

        // Some sections are placed in a sorted order. Hot items are placed first,
        // then cold items. These sections are marked as HotColdSortedRange since
        // they are neither completely hot, nor completely cold. 
        m_pVirtualImportThunkSection        = NewVirtualSection(pXDataSection, IBCProfiledSection | HotColdSortedRange | VirtualImportThunkSection, HELPER_TABLE_ALIGN);
        m_pExternalMethodThunkSection       = NewVirtualSection(pXDataSection, IBCProfiledSection | HotColdSortedRange | ExternalMethodThunkSection, HELPER_TABLE_ALIGN);
        m_pHelperTableSection               = NewVirtualSection(pXDataSection, IBCProfiledSection | HotColdSortedRange| HelperTableSection, HELPER_TABLE_ALIGN);

        // hot for writing, i.e. profiling has indicated a write to this item, so at least one write likely per item at some point
        m_pPreloadSections[CORCOMPILE_SECTION_METHOD_PRECODE_WRITE] = NewVirtualSection(pXDataSection, IBCProfiledSection | HotRange | MethodPrecodeWriteSection, sizeof(TADDR));
        m_pPreloadSections[CORCOMPILE_SECTION_METHOD_PRECODE_HOT] = NewVirtualSection(pXDataSection, IBCProfiledSection | HotRange | MethodPrecodeSection, sizeof(TADDR));

        //
        // cold sections
        //
        m_pPreloadSections[CORCOMPILE_SECTION_METHOD_PRECODE_COLD] = NewVirtualSection(pXDataSection, IBCProfiledSection | ColdRange | MethodPrecodeSection, sizeof(TADDR));
        m_pPreloadSections[CORCOMPILE_SECTION_METHOD_PRECODE_COLD_WRITEABLE] = NewVirtualSection(pXDataSection, IBCProfiledSection | ColdRange | MethodPrecodeWriteableSection, sizeof(TADDR));
    }

    {
        // code:NativeUnwindInfoLookupTable::LookupUnwindInfoForMethod and code:NativeImageJitManager::GetFunctionEntry expects 
        // sentinel value right after end of .pdata section. 
        static const DWORD dwRuntimeFunctionSectionSentinel = (DWORD)-1;


        //
        // .text section
        //
#if defined(_TARGET_ARM_)
        // for ARM, put the resource section at the end if it's very large - this
        // is because b and bl instructions have a limited distance range of +-16MB
        // which we should not exceed if we can avoid it.
        // we draw the limit at 1 MB resource size, somewhat arbitrarily
        COUNT_T resourceSize;
        m_ModuleDecoder.GetResources(&resourceSize);
        BOOL bigResourceSection = resourceSize >= 1024*1024;
#endif
        ZapPhysicalSection * pTextSection = NewPhysicalSection(".text", IMAGE_SCN_CNT_CODE | TEXT_MEM);
        m_pTextSection = pTextSection;

        // Marked as HotRange since it contains items that are always touched by
        // the OS during NGEN image loading (i.e. VersionInfo) 
        m_pWin32ResourceSection = NewVirtualSection(pTextSection, IBCUnProfiledSection | HotRange | Win32ResourcesSection);

        // Marked as a HotRange since it is always touched during Ngen image load. 
        m_pHeaderSection = NewVirtualSection(pTextSection, IBCUnProfiledSection | HotRange | HeaderSection);

        // Marked as a HotRange since it is always touched during Ngen image binding.
        m_pMetaDataSection = NewVirtualSection(pTextSection, IBCUnProfiledSection | HotRange | MetadataSection);

        m_pImportTableSection = NewVirtualSection(pTextSection, IBCUnProfiledSection | HotRange | ImportTableSection, sizeof(DWORD));

        m_pDelayLoadInfoDelayListSectionEager = NewVirtualSection(pTextSection, IBCUnProfiledSection | HotRange | DelayLoadInfoDelayListSection, sizeof(DWORD));

        //
        // GC Info for methods which were profiled hot AND had their GC Info touched during profiling
        //
        m_pHotTouchedGCSection = NewVirtualSection(pTextSection, IBCProfiledSection | HotRange | GCInfoSection, sizeof(DWORD));

        m_pLazyHelperSection = NewVirtualSection(pTextSection, IBCUnProfiledSection | HotRange | HelperTableSection, MINIMUM_CODE_ALIGN);
        m_pLazyHelperSection->SetDefaultFill(DEFAULT_CODE_BUFFER_INIT);

        m_pLazyMethodCallHelperSection = NewVirtualSection(pTextSection, IBCUnProfiledSection | HotRange | HelperTableSection, MINIMUM_CODE_ALIGN);
        m_pLazyMethodCallHelperSection->SetDefaultFill(DEFAULT_CODE_BUFFER_INIT);

        int codeSectionAlign = DEFAULT_CODE_ALIGN;

        m_pHotCodeSection = NewVirtualSection(pTextSection, IBCProfiledSection | HotRange | CodeSection, codeSectionAlign);
        m_pHotCodeSection->SetDefaultFill(DEFAULT_CODE_BUFFER_INIT);

#if defined(WIN64EXCEPTIONS)
        m_pHotUnwindDataSection = NewVirtualSection(pTextSection, IBCProfiledSection | HotRange | UnwindDataSection, sizeof(DWORD)); // .rdata area

        // All RuntimeFunctionSections have to be together for WIN64EXCEPTIONS
        m_pHotRuntimeFunctionSection = NewVirtualSection(pTextSection, IBCProfiledSection | HotRange | RuntimeFunctionSection, sizeof(DWORD));  // .pdata area
        m_pRuntimeFunctionSection = NewVirtualSection(pTextSection, IBCProfiledSection | WarmRange  | ColdRange | RuntimeFunctionSection, sizeof(DWORD));
        m_pColdRuntimeFunctionSection = NewVirtualSection(pTextSection, IBCProfiledSection | IBCUnProfiledSection | ColdRange | RuntimeFunctionSection, sizeof(DWORD));

        // The following sentinel section is just a padding for RuntimeFunctionSection - Apply same classification 
        NewVirtualSection(pTextSection, IBCProfiledSection | IBCUnProfiledSection | ColdRange | RuntimeFunctionSection, sizeof(DWORD))
            ->Place(new (GetHeap()) ZapBlobPtr((PVOID)&dwRuntimeFunctionSectionSentinel, sizeof(DWORD)));
#endif  // defined(WIN64EXCEPTIONS)

        m_pStubsSection = NewVirtualSection(pTextSection, IBCProfiledSection | HotColdSortedRange | StubsSection);
        m_pReadOnlyDataSection = NewVirtualSection(pTextSection, IBCProfiledSection | HotColdSortedRange | ReadonlyDataSection);

        m_pDynamicHelperDataSection = NewVirtualSection(pTextSection, IBCProfiledSection | HotColdSortedRange | ExternalMethodDataSection, sizeof(DWORD));
        m_pExternalMethodDataSection = NewVirtualSection(pTextSection, IBCProfiledSection | HotColdSortedRange | ExternalMethodDataSection, sizeof(DWORD));
        m_pStubDispatchDataSection = NewVirtualSection(pTextSection, IBCProfiledSection | HotColdSortedRange | StubDispatchDataSection, sizeof(DWORD));

        m_pHotRuntimeFunctionLookupSection = NewVirtualSection(pTextSection, IBCProfiledSection | HotRange | RuntimeFunctionSection, sizeof(DWORD));
#if !defined(WIN64EXCEPTIONS)
        m_pHotRuntimeFunctionSection = NewVirtualSection(pTextSection, IBCProfiledSection | HotRange | RuntimeFunctionSection, sizeof(DWORD));

        // The following sentinel section is just a padding for RuntimeFunctionSection - Apply same classification 
        NewVirtualSection(pTextSection, IBCProfiledSection | HotRange | RuntimeFunctionSection, sizeof(DWORD))
            ->Place(new (GetHeap()) ZapBlobPtr((PVOID)&dwRuntimeFunctionSectionSentinel, sizeof(DWORD)));
#endif
        m_pHotCodeMethodDescsSection = NewVirtualSection(pTextSection, IBCProfiledSection | HotRange | CodeManagerSection, sizeof(DWORD));

        m_pDelayLoadInfoDelayListSectionHot = NewVirtualSection(pTextSection, IBCProfiledSection | HotRange | DelayLoadInfoDelayListSection, sizeof(DWORD));

        //
        // The hot set of read-only data structures.  Note that read-only data structures are the things that we can (and aggressively do) intern
        // to share between different owners.  However, this can have a bad interaction with IBC, which performs its ordering optimizations without
        // knowing that NGen may jumble around layout with interning.  Thankfully, it is a relatively small percentage of the items that are duplicates
        // (many of them used a great deal to add up to large interning savings).  This means that we can track all of the interned items for which we
        // actually find any duplicates and put those in a small section.  For the rest, where there wasn't a duplicate in the entire image, we leave the
        // singleton in its normal place in the READONLY_HOT section, which was selected carefully by IBC.
        //
        m_pPreloadSections[CORCOMPILE_SECTION_READONLY_SHARED_HOT] = NewVirtualSection(pTextSection, IBCProfiledSection | HotRange | ReadonlySharedSection, sizeof(TADDR));
        m_pPreloadSections[CORCOMPILE_SECTION_READONLY_HOT] = NewVirtualSection(pTextSection, IBCProfiledSection | HotRange | ReadonlySection, sizeof(TADDR));

        //
        // GC Info for methods which were touched during profiling but didn't explicitly have
        // their GC Info touched during profiling
        //
        m_pHotGCSection = NewVirtualSection(pTextSection, IBCProfiledSection | WarmRange | GCInfoSection, sizeof(DWORD));

#if !defined(_TARGET_ARM_)
        // For ARM, put these sections more towards the end because bl/b instructions have limited diplacement

        // IL
        m_pILSection  = NewVirtualSection(pTextSection, IBCProfiledSection | HotColdSortedRange | ILSection, sizeof(DWORD));

        //ILMetadata/Resources sections are reported as a statically known warm ranges for now.
        m_pILMetaDataSection = NewVirtualSection(pTextSection, IBCProfiledSection | HotColdSortedRange | ILMetadataSection, sizeof(DWORD));
#endif  // _TARGET_ARM

#if defined(_TARGET_ARM_)
        if (!bigResourceSection) // for ARM, put the resource section at the end if it's very large - see comment above
#endif
            m_pResourcesSection = NewVirtualSection(pTextSection, IBCUnProfiledSection | WarmRange | ResourcesSection);

        //
        // Allocate the unprofiled code section and code manager nibble map here
        //
        m_pCodeSection = NewVirtualSection(pTextSection, IBCProfiledSection | WarmRange | ColdRange | CodeSection, codeSectionAlign);
        m_pCodeSection->SetDefaultFill(DEFAULT_CODE_BUFFER_INIT);

        m_pRuntimeFunctionLookupSection = NewVirtualSection(pTextSection, IBCProfiledSection | WarmRange | ColdRange | RuntimeFunctionSection, sizeof(DWORD));
#if !defined(WIN64EXCEPTIONS)
        m_pRuntimeFunctionSection = NewVirtualSection(pTextSection, IBCProfiledSection | WarmRange  | ColdRange | RuntimeFunctionSection, sizeof(DWORD));

        // The following sentinel section is just a padding for RuntimeFunctionSection - Apply same classification 
        NewVirtualSection(pTextSection, IBCProfiledSection | WarmRange  | ColdRange | RuntimeFunctionSection, sizeof(DWORD))
            ->Place(new (GetHeap()) ZapBlobPtr((PVOID)&dwRuntimeFunctionSectionSentinel, sizeof(DWORD)));
#endif
        m_pCodeMethodDescsSection = NewVirtualSection(pTextSection, IBCProfiledSection | WarmRange | ColdRange | CodeHeaderSection,sizeof(DWORD));

#if defined(WIN64EXCEPTIONS)
        m_pUnwindDataSection = NewVirtualSection(pTextSection, IBCProfiledSection | WarmRange | ColdRange | UnwindDataSection, sizeof(DWORD));
#endif // defined(WIN64EXCEPTIONS)

        m_pPreloadSections[CORCOMPILE_SECTION_READONLY_WARM] = NewVirtualSection(pTextSection, IBCProfiledSection | WarmRange | ReadonlySection, sizeof(TADDR));

        //
        // GC Info for methods which were not touched in profiling
        //
        m_pGCSection = NewVirtualSection(pTextSection, IBCProfiledSection | ColdRange | GCInfoSection, sizeof(DWORD));

        m_pDelayLoadInfoDelayListSectionCold = NewVirtualSection(pTextSection, IBCProfiledSection | ColdRange | DelayLoadInfoDelayListSection, sizeof(DWORD));

        m_pPreloadSections[CORCOMPILE_SECTION_READONLY_COLD] = NewVirtualSection(pTextSection, IBCProfiledSection | ColdRange | ReadonlySection, sizeof(TADDR));

        //
        // Allocate the cold code section near the end of the image
        //
        m_pColdCodeSection = NewVirtualSection(pTextSection, IBCProfiledSection | IBCUnProfiledSection | ColdRange | CodeSection, codeSectionAlign);
        m_pColdCodeSection->SetDefaultFill(DEFAULT_CODE_BUFFER_INIT);

#if defined(_TARGET_ARM_)
        // For ARM, put these sections more towards the end because bl/b instructions have limited diplacement

        // IL
        m_pILSection  = NewVirtualSection(pTextSection, IBCProfiledSection | HotColdSortedRange | ILSection, sizeof(DWORD));

        //ILMetadata/Resources sections are reported as a statically known warm ranges for now.
        m_pILMetaDataSection = NewVirtualSection(pTextSection, IBCProfiledSection | HotColdSortedRange | ILMetadataSection, sizeof(DWORD));

        if (bigResourceSection) // for ARM, put the resource section at the end if it's very large - see comment above
            m_pResourcesSection = NewVirtualSection(pTextSection, IBCUnProfiledSection | WarmRange | ResourcesSection);
#endif // _TARGET_ARM_
        m_pColdCodeMapSection = NewVirtualSection(pTextSection, IBCProfiledSection | IBCUnProfiledSection | ColdRange | CodeManagerSection, sizeof(DWORD));

#if !defined(WIN64EXCEPTIONS)
        m_pColdRuntimeFunctionSection = NewVirtualSection(pTextSection, IBCProfiledSection | IBCUnProfiledSection | ColdRange | RuntimeFunctionSection, sizeof(DWORD));

        // The following sentinel section is just a padding for RuntimeFunctionSection - Apply same classification 
        NewVirtualSection(pTextSection, IBCProfiledSection | IBCUnProfiledSection | ColdRange | RuntimeFunctionSection, sizeof(DWORD))
            ->Place(new (GetHeap()) ZapBlobPtr((PVOID)&dwRuntimeFunctionSectionSentinel, sizeof(DWORD)));
#endif

#if defined(WIN64EXCEPTIONS)
        m_pColdUnwindDataSection = NewVirtualSection(pTextSection, IBCProfiledSection | IBCUnProfiledSection | ColdRange | UnwindDataSection, sizeof(DWORD));
#endif // defined(WIN64EXCEPTIONS)

        //
        // Allocate space for compressed LookupMaps (ridmaps). This needs to come after the .data physical
        // section (which is currently true for the .text section) and late enough in the .text section to be
        // after any structure referenced by the LookupMap (current MethodTables and MethodDescs). This is a
        // hard requirement since the compression algorithm requires that all referenced data structures have
        // been laid out by the time we come to lay out the compressed nodes.
        //
        m_pPreloadSections[CORCOMPILE_SECTION_COMPRESSED_MAPS] = NewVirtualSection(pTextSection, IBCProfiledSection | ColdRange | CompressedMapsSection, sizeof(DWORD));

        m_pExceptionSection = NewVirtualSection(pTextSection, IBCProfiledSection | HotColdSortedRange | ExceptionSection, sizeof(DWORD));

        //
        // Debug info is sometimes used during exception handling to build stacktrace
        //
        m_pDebugSection = NewVirtualSection(pTextSection, IBCUnProfiledSection | ColdRange | DebugSection, sizeof(DWORD));
    }

#ifdef MDIL
    {
        //
        // .mdil section
        //
        m_pMDILSection = NULL;
        if (m_zapper->m_fEmbedMDIL)
        {
            ZapPhysicalSection * pMDILSection = NewPhysicalSection(".mdil", IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_DISCARDABLE | IMAGE_SCN_MEM_READ);
            m_pMDILSection = NewVirtualSection(pMDILSection, IBCUnProfiledSection | ColdRange | MDILDataSection);
        }
    }
#endif

    {
        //
        // .reloc section
        //

        ZapPhysicalSection * pRelocSection = NewPhysicalSection(".reloc", IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_DISCARDABLE | IMAGE_SCN_MEM_READ);

        // .reloc section is always read by the OS when the image is opted in ASLR
        // (Vista+ default behavior). 
        m_pBaseRelocsSection = NewVirtualSection(pRelocSection, IBCUnProfiledSection | HotRange | BaseRelocsSection);

    }
}

void ZapImage::Preallocate()
{
    COUNT_T cbILImage = m_ModuleDecoder.GetSize();

    // Curb the estimate to handle corner cases gracefuly
    cbILImage = min(cbILImage, 50000000);

    PREALLOCATE_HASHTABLE(ZapImage::m_CompiledMethods, 0.0050, cbILImage);
    PREALLOCATE_HASHTABLE(ZapImage::m_ClassLayoutOrder, 0.0003, cbILImage);

    //
    // Preallocation of auxiliary tables in alphabetical order
    //
    m_pImportTable->Preallocate(cbILImage);
    m_pInnerPtrs->Preallocate(cbILImage);
    m_pMethodEntryPoints->Preallocate(cbILImage);
    m_pWrappers->Preallocate(cbILImage);

#ifndef BINDER
    if (m_pILMetaData != NULL)
        m_pILMetaData->Preallocate(cbILImage);
#endif
    m_pGCInfoTable->Preallocate(cbILImage);
#ifdef WIN64EXCEPTIONS
    m_pUnwindDataTable->Preallocate(cbILImage);
#endif // WIN64EXCEPTIONS
    m_pDebugInfoTable->Preallocate(cbILImage);
}

#ifdef BINDER
void ZapImage::SetNativeVersionResource(PVOID pvVersionResourceBlob, SIZE_T cbVersionResource)
{
    ZapNode* pBlob = ZapBlob::NewAlignedBlob(this, pvVersionResourceBlob, cbVersionResource, sizeof(TADDR));
    ZapVersionResource * pWin32VersionResource = new (GetHeap()) ZapVersionResource(pBlob);
    m_pWin32ResourceSection->Place(pWin32VersionResource);
    m_pWin32ResourceSection->Place(pBlob);

    SetDirectoryEntry(IMAGE_DIRECTORY_ENTRY_RESOURCE, m_pWin32ResourceSection);
}
#endif
#ifdef CLR_STANDALONE_BINDER
void ZapImage::EmitMethodIL(mdToken methodDefToken)
{
    if (m_pILMetaData != NULL)
        m_pILMetaData->EmitMethodIL(methodDefToken);
}
void ZapImage::EmitFieldRVA(mdToken fieldDefToken, RVA fieldRVA)
{
    if (m_pILMetaData != NULL)
        m_pILMetaData->EmitFieldRVA(fieldDefToken, fieldRVA);
}
#endif

void ZapImage::SetVersionInfo(CORCOMPILE_VERSION_INFO * pVersionInfo)
{
    m_pVersionInfo = new (GetHeap()) ZapVersionInfo(pVersionInfo);
    m_pHeaderSection->Place(m_pVersionInfo);
}

void ZapImage::SetDependencies(CORCOMPILE_DEPENDENCY *pDependencies, DWORD cDependencies)
{
    m_pDependencies = new (GetHeap()) ZapDependencies(pDependencies, cDependencies);
    m_pHeaderSection->Place(m_pDependencies);
}

void ZapImage::SetPdbFileName(const SString &strFileName)
{
    m_pdbFileName.Set(strFileName);
}

#ifdef WIN64EXCEPTIONS
void ZapImage::SetRuntimeFunctionsDirectoryEntry()
{
    //
    // Runtime functions span multiple virtual sections and so there is no natural ZapNode * to cover them all.
    // Create dummy ZapNode * that covers them all for IMAGE_DIRECTORY_ENTRY_EXCEPTION directory entry.
    //
    ZapVirtualSection * rgRuntimeFunctionSections[] = {
        m_pHotRuntimeFunctionSection,
        m_pRuntimeFunctionSection,
        m_pColdRuntimeFunctionSection
    };

    DWORD dwTotalSize = 0, dwStartRVA = (DWORD)-1, dwEndRVA = 0;

    for (size_t i = 0; i < _countof(rgRuntimeFunctionSections); i++)
    {
        ZapVirtualSection * pSection = rgRuntimeFunctionSections[i];

        DWORD dwSize = pSection->GetSize();
        if (dwSize == 0)
            continue;

        DWORD dwRVA = pSection->GetRVA();

        dwTotalSize += dwSize;

        dwStartRVA = min(dwStartRVA, dwRVA);
        dwEndRVA = max(dwEndRVA, dwRVA + dwSize);
    }

    if (dwTotalSize != 0)
    {
        // Verify that there are no holes between the sections
        _ASSERTE(dwStartRVA + dwTotalSize == dwEndRVA);

        ZapNode * pAllRuntimeFunctionSections = new (GetHeap()) ZapDummyNode(dwTotalSize);
        pAllRuntimeFunctionSections->SetRVA(dwStartRVA);

        // Write the address of the sorted pdata to the optionalHeader.DataDirectory
        SetDirectoryEntry(IMAGE_DIRECTORY_ENTRY_EXCEPTION, pAllRuntimeFunctionSections);
    }
}
#endif // WIN64EXCEPTIONS

// Assign RVAs to all ZapNodes
void ZapImage::ComputeRVAs()
{
    ZapWriter::ComputeRVAs();

    if (!IsReadyToRunCompilation())
    {
        m_pMethodEntryPoints->Resolve();
        m_pWrappers->Resolve();
    }

    m_pInnerPtrs->Resolve();

#ifdef WIN64EXCEPTIONS
    SetRuntimeFunctionsDirectoryEntry();
#endif

#if defined(_DEBUG) 
#ifdef FEATURE_SYMDIFF
    if (CLRConfig::GetConfigValue(CLRConfig::INTERNAL_SymDiffDump))
    {
        COUNT_T curMethod = 0;
        COUNT_T numMethods = m_MethodCompilationOrder.GetCount();

        for (; curMethod < numMethods; curMethod++)
        {
            bool fCold = false;
            //if(curMethod >= m_iUntrainedMethod) fCold = true;
    		
            ZapMethodHeader * pMethod = m_MethodCompilationOrder[curMethod];

            ZapBlobWithRelocs * pCode = fCold ? pMethod->m_pColdCode : pMethod->m_pCode;
            if (pCode == NULL)
            {            
                continue;
            }
            CORINFO_METHOD_HANDLE handle = pMethod->GetHandle();
            mdMethodDef token;
            GetCompileInfo()->GetMethodDef(handle, &token);
            GetSvcLogger()->Printf(W("(EntryPointRVAMap (MethodToken %0X) (RVA %0X) (SIZE %0X))\n"), token, pCode->GetRVA(), pCode->GetSize()); 
        }

    }
#endif // FEATURE_SYMDIFF 
#endif //_DEBUG
}

class ZapFileStream : public IStream
{
    HANDLE  m_hFile;
    MD5 m_hasher;

public:
    ZapFileStream()
        : m_hFile(INVALID_HANDLE_VALUE)
    {
        m_hasher.Init();
    }

    ~ZapFileStream()
    {
        Close();
    }

    void SetHandle(HANDLE hFile)
    {
        _ASSERTE(m_hFile == INVALID_HANDLE_VALUE);
        m_hFile = hFile;
    }

    // IUnknown methods:
    STDMETHODIMP_(ULONG) AddRef()
    {
        return 1;
    }

    STDMETHODIMP_(ULONG) Release()
    {
        return 1;
    }

    STDMETHODIMP QueryInterface(REFIID riid, LPVOID *ppv)
    {
        HRESULT hr = S_OK;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IStream)) {
            *ppv = static_cast<IStream *>(this);
        }
        else {
            hr = E_NOINTERFACE;
        }
        return hr;
    }

    // ISequentialStream methods:
    STDMETHODIMP Read(void *pv, ULONG cb, ULONG *pcbRead)
    {
        _ASSERTE(false);
        return E_NOTIMPL;
    }

    STDMETHODIMP Write(void const *pv, ULONG cb, ULONG *pcbWritten)
    {
        HRESULT hr = S_OK;

        _ASSERTE(m_hFile != INVALID_HANDLE_VALUE);

        m_hasher.HashMore(pv, cb);

        if (!::WriteFile(m_hFile, pv, cb, pcbWritten, NULL))
        {
            hr = HRESULT_FROM_GetLastError();
            goto Exit;
        }

    Exit:
        return hr;
    }

    // IStream methods:
    STDMETHODIMP Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER *plibNewPosition)
    {
        HRESULT hr = S_OK;        

        _ASSERTE(m_hFile != INVALID_HANDLE_VALUE);

        DWORD dwFileOrigin;
        switch (dwOrigin) {
            case STREAM_SEEK_SET:
                dwFileOrigin = FILE_BEGIN;
                break;
                
            case STREAM_SEEK_CUR:
                dwFileOrigin = FILE_CURRENT;
                break;
                
            case STREAM_SEEK_END:
                dwFileOrigin = FILE_END;
                break;
                
            default:
                hr = E_UNEXPECTED;
                goto Exit;
        }
        if (!::SetFilePointerEx(m_hFile, dlibMove, (LARGE_INTEGER *)plibNewPosition, dwFileOrigin))
        {
            hr = HRESULT_FROM_GetLastError();
            goto Exit;
        }

    Exit:
        return hr;
    }

    STDMETHODIMP SetSize(ULARGE_INTEGER libNewSize)
    {
        HRESULT hr = S_OK;

        _ASSERTE(m_hFile != INVALID_HANDLE_VALUE);

        hr = Seek(*(LARGE_INTEGER *)&libNewSize, FILE_BEGIN, NULL);
        if (FAILED(hr))
        {
            goto Exit;
        }

        if (!::SetEndOfFile(m_hFile))
        {
            hr = HRESULT_FROM_GetLastError();
            goto Exit;
        }

    Exit:
        return hr;
    }

    STDMETHODIMP CopyTo(IStream *pstm, ULARGE_INTEGER cb, ULARGE_INTEGER *pcbRead, ULARGE_INTEGER *pcbWritten)
    {
        _ASSERTE(false);
        return E_NOTIMPL;
    }

    STDMETHODIMP Commit(DWORD grfCommitFlags)
    {
        _ASSERTE(false);
        return E_NOTIMPL;
    }

    STDMETHODIMP Revert()
    {
        _ASSERTE(false);
        return E_NOTIMPL;
    }

    STDMETHODIMP LockRegion(ULARGE_INTEGER libOffset, ULARGE_INTEGER cb, DWORD dwLockType)
    {
        _ASSERTE(false);
        return E_NOTIMPL;
    }

    STDMETHODIMP UnlockRegion(ULARGE_INTEGER libOffset, ULARGE_INTEGER cb, DWORD dwLockType)
    {
        _ASSERTE(false);
        return E_NOTIMPL;
    }

    STDMETHODIMP Stat(STATSTG *pstatstg, DWORD grfStatFlag)
    {
        _ASSERTE(false);
        return E_NOTIMPL;
    }

    STDMETHODIMP Clone(IStream **ppIStream)
    {
        _ASSERTE(false);
        return E_NOTIMPL;
    }

    HRESULT Close()
    {
        HRESULT hr = S_OK;

        HANDLE hFile = m_hFile;
        if (hFile != INVALID_HANDLE_VALUE)
        {
            m_hFile = INVALID_HANDLE_VALUE;

            if (!::CloseHandle(hFile))
            {
                hr = HRESULT_FROM_GetLastError();
                goto Exit;
            }
        }

    Exit:
        return hr;
    }

    void SuppressClose()
    {
        m_hFile = INVALID_HANDLE_VALUE;
    }

    void GetHash(MD5HASHDATA* pHash)
    {
        m_hasher.GetHashValue(pHash);
    }
};

HANDLE ZapImage::GenerateFile(LPCWSTR wszOutputFileName, CORCOMPILE_NGEN_SIGNATURE * pNativeImageSig)
{
    ZapFileStream outputStream;

    HANDLE hFile = WszCreateFile(wszOutputFileName,
                        GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_DELETE,
                        NULL,
                        CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                        NULL);

    if (hFile == INVALID_HANDLE_VALUE)
        ThrowLastError();

    outputStream.SetHandle(hFile);

    Save(&outputStream);

    LARGE_INTEGER filePos;

    if (m_pNativeHeader != NULL)
    {
        // Write back the updated CORCOMPILE_HEADER (relocs and guid is not correct the first time around)
        filePos.QuadPart = m_pTextSection->GetFilePos() + 
            (m_pNativeHeader->GetRVA() - m_pTextSection->GetRVA());
        IfFailThrow(outputStream.Seek(filePos, STREAM_SEEK_SET, NULL));
        m_pNativeHeader->Save(this);
        FlushWriter();
    }

    GUID signature = {0};

    static_assert_no_msg(sizeof(GUID) == sizeof(MD5HASHDATA));
    outputStream.GetHash((MD5HASHDATA*)&signature);

    {    
        // Write the debug directory entry for the NGEN PDB
        RSDS rsds = {0};
        
        rsds.magic = 'SDSR';
        rsds.age = 1;
        // our PDB signature will be the same as our NGEN signature.  
        // However we want the printed version of the GUID to be be the same as the
        // byte dump of the signature so we swap bytes to make this work.  
        // 
        // * See code:CCorSvcMgr::CreatePdb for where this is used.
        BYTE* asBytes = (BYTE*) &signature;
        rsds.signature.Data1 = ((asBytes[0] * 256 + asBytes[1]) * 256 + asBytes[2]) * 256 + asBytes[3];
        rsds.signature.Data2 = asBytes[4] * 256 + asBytes[5];
        rsds.signature.Data3 = asBytes[6] * 256 + asBytes[7];
        memcpy(&rsds.signature.Data4, &asBytes[8], 8);

        _ASSERTE(!m_pdbFileName.IsEmpty());
        ZeroMemory(&rsds.path[0], sizeof(rsds.path));
        if (WideCharToMultiByte(CP_UTF8, 
                                0, 
                                m_pdbFileName.GetUnicode(),
                                m_pdbFileName.GetCount(), 
                                &rsds.path[0], 
                                sizeof(rsds.path) - 1, // -1 to keep the buffer zero terminated
                                NULL, 
                                NULL) == 0)
            ThrowHR(E_FAIL);
        
        ULONG cbWritten = 0;
        filePos.QuadPart = m_pTextSection->GetFilePos() + (m_pNGenPdbDebugData->GetRVA() - m_pTextSection->GetRVA());
        IfFailThrow(outputStream.Seek(filePos, STREAM_SEEK_SET, NULL));
        IfFailThrow(outputStream.Write(&rsds, sizeof rsds, &cbWritten));
    }

    if (m_pVersionInfo != NULL)
    {
        ULONG cbWritten;

        filePos.QuadPart = m_pTextSection->GetFilePos() + 
            (m_pVersionInfo->GetRVA() - m_pTextSection->GetRVA()) + 
            offsetof(CORCOMPILE_VERSION_INFO, signature);
        IfFailThrow(outputStream.Seek(filePos, STREAM_SEEK_SET, NULL));
        IfFailThrow(outputStream.Write(&signature, sizeof(signature), &cbWritten));

        if (pNativeImageSig != NULL)
            *pNativeImageSig = signature;
    }
    else
    {
        _ASSERTE(pNativeImageSig == NULL);
    }

    outputStream.SuppressClose();
    return hFile;
}

#ifdef FEATURE_FUSION
#define WOF_PROVIDER_FILE           (0x00000002)

typedef BOOL (WINAPI *WofShouldCompressBinaries_t) (
    __in LPCWSTR Volume,
    __out PULONG Algorithm
    );

typedef HRESULT (WINAPI *WofSetFileDataLocation_t) (
    __in HANDLE hFile,
    __out ULONG Provider,
    __in PVOID FileInfo,
    __in ULONG Length
    );

typedef struct _WOF_FILE_COMPRESSION_INFO {
    ULONG Algorithm;
} WOF_FILE_COMPRESSION_INFO, *PWOF_FILE_COMPRESSION_INFO;

// Check if files on the volume identified by volumeLetter should be compressed.
// If yes, compress the file associated with hFile.
static void CompressFile(WCHAR volumeLetter, HANDLE hFile)
{
    if (IsNgenOffline())
    {
        return;
    }

    // Wofutil.dll is available on Windows 8.1 and above. Return on platforms without wofutil.dll.
    HModuleHolder wofLibrary(WszLoadLibraryEx(L"wofutil.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32));
    if (wofLibrary == nullptr)
    {
        return;
    }

    // WofShouldCompressBinaries is available on Windows 10 and above.
    // Windows 8.1 version of wofutil.dll does not have this function.
    WofShouldCompressBinaries_t WofShouldCompressBinaries
        = (WofShouldCompressBinaries_t)GetProcAddress(wofLibrary, "WofShouldCompressBinaries");
    if (WofShouldCompressBinaries == nullptr)
    {
        return;
    }

    WCHAR volume[4] = L"X:\\";
    volume[0] = volumeLetter;
    ULONG algorithm = 0;

    bool compressionSuitable = (WofShouldCompressBinaries(volume, &algorithm) == TRUE);
    if (compressionSuitable)
    {
        // WofSetFileDataLocation is available on Windows 8.1 and above, however, Windows 8.1 version
        // of WofSetFileDataLocation works for WIM only, and Windows 10 is required for compression of
        // normal files.  This isn't a problem for us, since the check for WofShouldCompressBinaries
        // above should have already returned on Windows 8.1.
        WofSetFileDataLocation_t WofSetFileDataLocation = 
            (WofSetFileDataLocation_t)GetProcAddress(wofLibrary, "WofSetFileDataLocation");
        if (WofSetFileDataLocation == nullptr)
        {
            return;
        }

        WOF_FILE_COMPRESSION_INFO fileInfo;
        fileInfo.Algorithm = algorithm;

        WofSetFileDataLocation(hFile, WOF_PROVIDER_FILE, &fileInfo, sizeof(WOF_FILE_COMPRESSION_INFO));
    }
}
#endif

HANDLE ZapImage::SaveImage(LPCWSTR wszOutputFileName, CORCOMPILE_NGEN_SIGNATURE * pNativeImageSig)
{
    if (!IsReadyToRunCompilation())
    {
        OutputManifestMetadata();
    }

    OutputTables();

    ComputeRVAs();

    if (!IsReadyToRunCompilation())
    {
        m_pPreloader->FixupRVAs();

#ifdef CLR_STANDALONE_BINDER
        m_pDataImage->FixupRVAs();
#endif
    }

    HANDLE hFile = GenerateFile(wszOutputFileName, pNativeImageSig);

#ifndef FEATURE_CORECLR
    if (m_stats != NULL)
        PrintStats(wszOutputFileName);
#endif

#ifdef FEATURE_FUSION
    CompressFile(wszOutputFileName[0], hFile);
#endif

    return hFile;
}

void ZapImage::PrintStats(LPCWSTR wszOutputFileName)
{
    m_stats->m_gcInfoSize = m_pHotTouchedGCSection->GetSize() + m_pHotGCSection->GetSize() + m_pGCSection->GetSize();
#if defined(WIN64EXCEPTIONS)
    m_stats->m_unwindInfoSize = m_pUnwindDataSection->GetSize() + 
        m_pHotRuntimeFunctionSection->GetSize() + m_pRuntimeFunctionSection->GetSize() + m_pColdRuntimeFunctionSection->GetSize();
#endif // defined(WIN64EXCEPTIONS)

    //
    // Get the size of the input & output files
    //

    {
        WIN32_FIND_DATA inputData;
        FindHandleHolder inputHandle = WszFindFirstFile(m_pModuleFileName, &inputData);
        if (inputHandle != INVALID_HANDLE_VALUE)
            m_stats->m_inputFileSize = inputData.nFileSizeLow;
    }

    {
        WIN32_FIND_DATA outputData;
        FindHandleHolder outputHandle = WszFindFirstFile(wszOutputFileName, &outputData);
        if (outputHandle != INVALID_HANDLE_VALUE)
            m_stats->m_outputFileSize = outputData.nFileSizeLow;
    }

    if (m_pAssemblyMetaData != NULL)
        m_stats->m_metadataSize = m_pAssemblyMetaData->GetSize();

    DWORD dwPreloadSize = 0;
    for (int iSection = 0; iSection < CORCOMPILE_SECTION_COUNT; iSection++)
        dwPreloadSize += m_pPreloadSections[iSection]->GetSize();
    m_stats->m_preloadImageSize = dwPreloadSize;

    m_stats->m_hotCodeMgrSize = m_pHotCodeMethodDescsSection->GetSize();
    m_stats->m_unprofiledCodeMgrSize = m_pCodeMethodDescsSection->GetSize();
    m_stats->m_coldCodeMgrSize = m_pHotRuntimeFunctionLookupSection->GetSize();

    m_stats->m_eeInfoTableSize = m_pEEInfoTable->GetSize();
    m_stats->m_helperTableSize = m_pHelperTableSection->GetSize();	
    m_stats->m_dynamicInfoTableSize = m_pImportSectionsTable->GetSize();
    m_stats->m_dynamicInfoDelayListSize = m_pDelayLoadInfoDelayListSectionEager->GetSize() + m_pDelayLoadInfoDelayListSectionHot->GetSize() + m_pDelayLoadInfoDelayListSectionCold->GetSize();
    m_stats->m_importTableSize = m_pImportTable->GetSize();

    m_stats->m_debuggingTableSize = m_pDebugSection->GetSize();
    m_stats->m_headerSectionSize = m_pGCSection->GetSize();
    m_stats->m_codeSectionSize = m_pHotCodeSection->GetSize();
    m_stats->m_coldCodeSectionSize = m_pColdCodeSection->GetSize();
    m_stats->m_exceptionSectionSize = m_pExceptionSection->GetSize();
    m_stats->m_readOnlyDataSectionSize = m_pReadOnlyDataSection->GetSize();
    m_stats->m_relocSectionSize =  m_pBaseRelocsSection->GetSize();
    if (m_pILMetaData != NULL)
        m_stats->m_ILMetadataSize = m_pILMetaData->GetSize();
    m_stats->m_virtualImportThunkSize = m_pVirtualImportThunkSection->GetSize();
    m_stats->m_externalMethodThunkSize = m_pExternalMethodThunkSection->GetSize();
    m_stats->m_externalMethodDataSize = m_pExternalMethodDataSection->GetSize();

    if (m_stats->m_failedMethods)
        m_zapper->Warning(W("Warning: %d methods (%d%%) could not be compiled.\n"),
                          m_stats->m_failedMethods, (m_stats->m_failedMethods*100) / m_stats->m_methods);
    if (m_stats->m_failedILStubs)
        m_zapper->Warning(W("Warning: %d IL STUB methods could not be compiled.\n"),
                          m_stats->m_failedMethods);
    m_stats->PrintStats();
}

// Align native images to 64K
const SIZE_T BASE_ADDRESS_ALIGNMENT  = 0xffff;
const double CODE_EXPANSION_FACTOR   =  3.6;

void ZapImage::CalculateZapBaseAddress()
{
    static SIZE_T nextBaseAddressForMultiModule;

    SIZE_T baseAddress = 0;

#ifndef BINDER // TritonTBD
    {
        // Read the actual preferred base address from the disk

        // Note that we are reopening the file here. We are not guaranteed to get the same file.
        // The worst thing that can happen is that we will read a bogus preferred base address from the file.
        HandleHolder hFile(WszCreateFile(m_pModuleFileName,
                                            GENERIC_READ,
                                            FILE_SHARE_READ|FILE_SHARE_DELETE,
                                            NULL,
                                            OPEN_EXISTING,
                                            FILE_ATTRIBUTE_NORMAL,
                                            NULL));
        if (hFile == INVALID_HANDLE_VALUE)
            ThrowLastError();

        HandleHolder hFileMap(WszCreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL));
        if (hFileMap == NULL)
            ThrowLastError();

        MapViewHolder base(MapViewOfFile(hFileMap, FILE_MAP_READ, 0, 0, 0));
        if (base == NULL)
            ThrowLastError();
    
        DWORD dwFileLen = SafeGetFileSize(hFile, 0);
        if (dwFileLen == INVALID_FILE_SIZE)
            ThrowLastError();

        PEDecoder peFlat((void *)base, (COUNT_T)dwFileLen);

        baseAddress = (SIZE_T) peFlat.GetPreferredBase();
    }

    // See if the header has the linker's default preferred base address
    if (baseAddress == (SIZE_T) 0x00400000)
    {
        if (m_fManifestModule)
        {
            // Set the base address for the main assembly with the manifest
        
            if (!m_ModuleDecoder.IsDll())
            {
#if defined(_TARGET_X86_)
                // We use 30000000 for an exe
                baseAddress = 0x30000000;
#elif defined(_WIN64)
                // We use 04000000 for an exe
                // which is remapped to 0x642`88000000 on x64
                baseAddress = 0x04000000;
#endif
            }
            else
            {
#if defined(_TARGET_X86_)
                // We start a 31000000 for the main assembly with the manifest
                baseAddress = 0x31000000;
#elif defined(_WIN64)
                // We start a 05000000 for the main assembly with the manifest
                // which is remapped to 0x642`8A000000 on x64
                baseAddress = 0x05000000;
#endif
            }
        }
        else // is dependent assembly of a multi-module assembly
        {
            // Set the base address for a dependant multi module assembly
                
            // We should have already set the nextBaseAddressForMultiModule
            // when we compiled the manifest module
            _ASSERTE(nextBaseAddressForMultiModule != 0);
            baseAddress = nextBaseAddressForMultiModule;
        }
    }
    else 
    {
        //
        // For some assemblies we have to move the ngen image base address up
        // past the end of IL image so that that we don't have a conflict.
        //
        // CoreCLR currently always loads both the IL and the native image, so
        // move the native image out of the way.
#ifndef FEATURE_CORECLR
        if (!m_ModuleDecoder.IsDll() ||     // exes always get loaded to their preferred base address
            !m_ModuleDecoder.IsILOnly())    // since the IL (IJW) image will be loaded first
#endif // !FEATURE_CORECLR
        {
            baseAddress += m_ModuleDecoder.GetVirtualSize();
        }
    }

    // Round to a multiple of 64K
    // 64K is the allocation granularity of VirtualAlloc. (Officially this number is not a constant -
    // we should be querying the system for its allocation granularity, but we do this all over the place
    // currently.)

    baseAddress = (baseAddress + BASE_ADDRESS_ALIGNMENT) & ~BASE_ADDRESS_ALIGNMENT;

    //
    // Calculate the nextBaseAddressForMultiModule
    //
    SIZE_T tempBaseAddress = baseAddress;
    tempBaseAddress += (SIZE_T) (CODE_EXPANSION_FACTOR * (double) m_ModuleDecoder.GetVirtualSize());
    tempBaseAddress += BASE_ADDRESS_ALIGNMENT;
    tempBaseAddress = (tempBaseAddress + BASE_ADDRESS_ALIGNMENT) & ~BASE_ADDRESS_ALIGNMENT;
    
    nextBaseAddressForMultiModule = tempBaseAddress;

    //
    // Now we remap the 32-bit address range used for x86 and PE32 images into thre
    // upper address range used on 64-bit platforms
    //
#if USE_UPPER_ADDRESS
#if defined(_WIN64)
    if (baseAddress < 0x80000000)
    {
        if (baseAddress < 0x40000000)
            baseAddress += 0x40000000; // We map [00000000..3fffffff] to [642'80000000..642'ffffffff]
        else
            baseAddress -= 0x40000000; // We map [40000000..7fffffff] to [642'00000000..642'7fffffff]

        baseAddress *= UPPER_ADDRESS_MAPPING_FACTOR;
        baseAddress += CLR_UPPER_ADDRESS_MIN;
    }
#endif
#endif
#endif // TritonTBD


    // Apply the calculated base address.
    SetBaseAddress(baseAddress);

    m_NativeBaseAddress = baseAddress;
}

#ifdef  MDIL
static WORD ReadWord(BYTE *p)
{
    return  p[0] +
            p[1]*256;
}

static DWORD ReadDWord(BYTE *p)
{
    return  p[0] + 
            p[1]*256 +
            p[2]*(256*256) +
            p[3]*(256*256*256);
}

#ifdef CLR_STANDALONE_BINDER
#include "mdil.h"
#else
#define CLR_STANDALONE_BINDER
#include "mdil.h"
#undef CLR_STANDALONE_BINDER
#endif

bool ReadMemory(BYTE *&dataPtr, COUNT_T &dataSize, void *dest, COUNT_T size)
{
    if (dataSize < size)
        return false;

    if (dest != NULL)
        memcpy(dest, dataPtr, size);

    dataPtr += size;
    dataSize -= size;

    return true;
}

void ZapImage::LoadMDILSection()
{
#ifdef BINDER
    _ASSERTE(!"intentionally unreachable");
#else
    IMAGE_SECTION_HEADER *pMDILSection = m_ModuleDecoder.FindSection(".mdil");
    m_cbMdilPESectionData = 0;
    if (pMDILSection)
    {
        // We got our section - get the start of the section
        BYTE* pStartOfMDILSection = static_cast<BYTE*>(m_ModuleDecoder.GetBase())+pMDILSection->VirtualAddress;
        BYTE* pEndOfMDILSection = pStartOfMDILSection + pMDILSection->Misc.VirtualSize;
        if (m_ModuleDecoder.PointerInPE(pEndOfMDILSection - 1))
        {
            m_pMdilPESectionData = pStartOfMDILSection;
            m_cbMdilPESectionData = pMDILSection->Misc.VirtualSize;
        }
    }
#endif
}

#endif // ifdef MDIL

void ZapImage::Open(CORINFO_MODULE_HANDLE hModule,
                        IMetaDataAssemblyEmit *pEmit)
{
    m_hModule   = hModule;
    m_fManifestModule = (hModule == m_zapper->m_pEECompileInfo->GetAssemblyModule(m_zapper->m_hAssembly));

    m_ModuleDecoder = *m_zapper->m_pEECompileInfo->GetModuleDecoder(hModule);

#ifdef FEATURE_FUSION
    // If TranslatePEToArchitectureType fails then we have an invalid format
    DWORD dwPEKind, dwMachine;
    m_ModuleDecoder.GetPEKindAndMachine(&dwPEKind, &dwMachine);

    PEKIND PeKind;
    IfFailThrow(TranslatePEToArchitectureType((CorPEKind)dwPEKind, dwMachine, &PeKind));
    
    // Valid images for this platform are peMSIL and the native image for the platform
    if (!(PeKind == peMSIL
#if defined(_TARGET_AMD64_)
          || PeKind == peAMD64
#elif defined(_TARGET_X86_)
          || PeKind == peI386
#elif defined(_TARGET_ARM_)
          || PeKind == peARM
#endif
        ))
    {
        ThrowHR(NGEN_E_EXE_MACHINE_TYPE_MISMATCH);
    }
#endif // FEATURE_FUSION

    //
    // Get file name, and base address from module
    //

    StackSString moduleFileName;
    m_zapper->m_pEECompileInfo->GetModuleFileName(hModule, moduleFileName);

    DWORD fileNameLength = moduleFileName.GetCount();
    m_pModuleFileName = new WCHAR[fileNameLength+1];
    wcscpy_s(m_pModuleFileName, fileNameLength+1, moduleFileName.GetUnicode());

    //
    // Load the IBC Profile data for the assembly if it exists
    // 
    LoadProfileData();

#ifdef  MDIL
#ifndef BINDER
    LoadMDILSection();
#endif
#endif
    //
    // Get metadata of module to be compiled
    //
    m_pMDImport = m_zapper->m_pEECompileInfo->GetModuleMetaDataImport(m_hModule);
#ifndef BINDER
    _ASSERTE(m_pMDImport != NULL);
#endif // !BINDER

    //
    // Open new assembly metadata data for writing.  We may not use it,
    // if so we'll just discard it at the end.
    //
    if (pEmit != NULL)
    {
        pEmit->AddRef();
        m_pAssemblyEmit = pEmit;
    }
    else
    {
        // Hardwire the metadata version to be the current runtime version so that the ngen image
        // does not change when the directory runtime is installed in different directory (e.g. v2.0.x86chk vs. v2.0.80826).
        BSTRHolder strVersion(SysAllocString(W("v")VER_PRODUCTVERSION_NO_QFE_STR_L));
        VARIANT versionOption;
        V_VT(&versionOption) = VT_BSTR;
        V_BSTR(&versionOption) = strVersion;
        IfFailThrow(m_zapper->m_pMetaDataDispenser->SetOption(MetaDataRuntimeVersion, &versionOption));

        IfFailThrow(m_zapper->m_pMetaDataDispenser->
                    DefineScope(CLSID_CorMetaDataRuntime, 0, IID_IMetaDataAssemblyEmit,
                                (IUnknown **) &m_pAssemblyEmit));
    }

#ifdef FEATURE_READYTORUN_COMPILER
    if (IsReadyToRunCompilation())
    {
        InitializeSectionsForReadyToRun();
    }
    else
#endif
    {
        InitializeSections();
    }

    // Set the module base address for the ngen native image
    CalculateZapBaseAddress();
}

#if !defined(FEATURE_CORECLR)

#if (_WIN32_WINNT < _WIN32_WINNT_WIN8)

typedef struct _WIN32_MEMORY_RANGE_ENTRY {

    PVOID VirtualAddress;
    SIZE_T NumberOfBytes;

} WIN32_MEMORY_RANGE_ENTRY, *PWIN32_MEMORY_RANGE_ENTRY;

#endif

typedef BOOL  
(WINAPI *PfnPrefetchVirtualMemory)(  
    _In_ HANDLE hProcess,  
    _In_ ULONG_PTR NumberOfEntries,  
    _In_reads_(NumberOfEntries) PWIN32_MEMORY_RANGE_ENTRY VirtualAddresses,  
    _In_ ULONG Flags  
    );  
  

void PrefetchVM(void * pStartAddress, SIZE_T size)
{
    static PfnPrefetchVirtualMemory s_pfnPrefetchVirtualMemory = NULL;  

    if (s_pfnPrefetchVirtualMemory == NULL)
    {
        s_pfnPrefetchVirtualMemory = (PfnPrefetchVirtualMemory) GetProcAddress(WszGetModuleHandle(WINDOWS_KERNEL32_DLLNAME_W), "PrefetchVirtualMemory");  

        if (s_pfnPrefetchVirtualMemory == NULL)
        {
            s_pfnPrefetchVirtualMemory = (PfnPrefetchVirtualMemory) (1);
        }
    }

    if (s_pfnPrefetchVirtualMemory > (PfnPrefetchVirtualMemory) (1))
    {
        WIN32_MEMORY_RANGE_ENTRY range;

        range.VirtualAddress = pStartAddress;
        range.NumberOfBytes  = size;

        s_pfnPrefetchVirtualMemory(GetCurrentProcess(), 1, & range, 0);
    }
}

#endif



//
// Load the module and populate all the data-structures
//

void ZapImage::Preload()
{
#if !defined(FEATURE_CORECLR)
    // Prefetch the whole IL image into memory to avoid small reads (usually 16kb blocks)
    PrefetchVM(m_ModuleDecoder.GetBase(), m_ModuleDecoder.GetSize());
#endif

    CorProfileData *  pProfileData = NewProfileData();
    m_pPreloader = m_zapper->m_pEECompileInfo->PreloadModule(m_hModule, this, pProfileData);
}

//
// Store the module
//

void ZapImage::LinkPreload()
{
    m_pPreloader->Link();
}

void ZapImage::OutputManifestMetadata()
{
    //
    // Write out manifest metadata
    //

    //
    // First, see if we have useful metadata to store
    //

    BOOL fMetadata = FALSE;

    if (m_pAssemblyEmit != NULL)
    {
        //
        // We may have added some assembly refs for exports.
        //

        NonVMComHolder<IMetaDataAssemblyImport> pAssemblyImport;
        IfFailThrow(m_pAssemblyEmit->QueryInterface(IID_IMetaDataAssemblyImport,
                                                    (void **)&pAssemblyImport));

        NonVMComHolder<IMetaDataImport> pImport;
        IfFailThrow(m_pAssemblyEmit->QueryInterface(IID_IMetaDataImport,
                                                    (void **)&pImport));

        HCORENUM hEnum = 0;
        ULONG cRefs;
        IfFailThrow(pAssemblyImport->EnumAssemblyRefs(&hEnum, NULL, 0, &cRefs));
        IfFailThrow(pImport->CountEnum(hEnum, &cRefs));
        pImport->CloseEnum(hEnum);

        if (cRefs > 0)
            fMetadata = TRUE;

        //
        // If we are the main module, we have the assembly def for the zap file.
        //

        mdAssembly a;
        if (pAssemblyImport->GetAssemblyFromScope(&a) == S_OK)
            fMetadata = TRUE;
    }

#ifdef CLR_STANDALONE_BINDER
    // TritonTBD:  A workaround to place a copy of metadata into hello.ni.exe.
    fMetadata = TRUE;
#endif

    if (fMetadata)
    {
#ifndef CLR_STANDALONE_BINDER
        // Metadata creates a new MVID for every instantiation.
        // However, we want the generated ngen image to always be the same
        // for the same input. So set the metadata MVID to NGEN_IMAGE_MVID.

        NonVMComHolder<IMDInternalEmit> pMDInternalEmit;
        IfFailThrow(m_pAssemblyEmit->QueryInterface(IID_IMDInternalEmit,
                                                  (void**)&pMDInternalEmit));

        IfFailThrow(pMDInternalEmit->ChangeMvid(NGEN_IMAGE_MVID));
#endif

        m_pAssemblyMetaData = new (GetHeap()) ZapMetaData();
        m_pAssemblyMetaData->SetMetaData(m_pAssemblyEmit);

#ifdef CLR_STANDALONE_BINDER

        // now generate the NativeAssembyManifest
        // push down first the assembly references
        // we can do this only AFTER we have an instance of ZapMetadata (see a few lines above)
        // the order of assembly references is/needs to be in sync with those in CORCOMPILE_DEPENDENCIES
        
        for (COUNT_T cnt = 0; cnt < m_pNativeManifestData.GetCount(); cnt++) {
            m_pAssemblyMetaData->SetAssemblyReference(
                     m_pNativeManifestData[cnt].m_AssemblyName,
                     NULL,
                     m_pNativeManifestData[cnt].m_pNad);
        }

        // now provide the assembly/module def relevant data
        // please note that his assumes/knows that the last assemblyRef is "self-referential"
        m_pAssemblyMetaData->SetAssembly(m_pNativeManifestData[(COUNT_T)m_selfIndex].m_AssemblyName,
                                         NULL,
                                         m_pNativeManifestData[(COUNT_T)m_selfIndex].m_pNad);

#endif

        m_pMetaDataSection->Place(m_pAssemblyMetaData);
    }
}

void ZapImage::OutputTables()
{
    //
    // Copy over any resources to the native image
    //

    COUNT_T size;
    PVOID resource = (PVOID)m_ModuleDecoder.GetResources(&size);

    if (size != 0)
    {
        m_pResources = new (GetHeap()) ZapBlobPtr(resource, size);
        m_pResourcesSection->Place(m_pResources);
    }

    CopyDebugDirEntry();
    CopyWin32VersionResource();

    if (m_pILMetaData != NULL)
    {
        m_pILMetaData->CopyIL();
        m_pILMetaData->CopyMetaData();
    }

    if (IsReadyToRunCompilation())
    {
        m_pILMetaData->CopyRVAFields();
    }

    // Copy over the timestamp from IL image for determinism
    SetTimeDateStamp(m_ModuleDecoder.GetTimeDateStamp());

    SetSubsystem(m_ModuleDecoder.GetSubsystem());

    {
        USHORT dllCharacteristics = 0;

#ifndef _WIN64
        dllCharacteristics |= IMAGE_DLLCHARACTERISTICS_NO_SEH;
#endif

#ifdef _TARGET_ARM_
        // Images without NX compat bit set fail to load on ARM
        dllCharacteristics |= IMAGE_DLLCHARACTERISTICS_NX_COMPAT;
#endif

        // Copy over selected DLL characteristics bits from IL image
        dllCharacteristics |= (m_ModuleDecoder.GetDllCharacteristics() & 
            (IMAGE_DLLCHARACTERISTICS_NX_COMPAT | IMAGE_DLLCHARACTERISTICS_TERMINAL_SERVER_AWARE | IMAGE_DLLCHARACTERISTICS_APPCONTAINER));

#ifdef _DEBUG
        if (0 == CLRConfig::GetConfigValue(CLRConfig::INTERNAL_NoASLRForNgen))
#endif // _DEBUG
        {
            dllCharacteristics |= IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE;
        }

        SetDllCharacteristics(dllCharacteristics);
    }

    if (IsReadyToRunCompilation())
    {
        SetIsDll(m_ModuleDecoder.IsDll());

        SetSizeOfStackReserve(m_ModuleDecoder.GetSizeOfStackReserve());
        SetSizeOfStackCommit(m_ModuleDecoder.GetSizeOfStackCommit());
    }

#if defined(_TARGET_ARM_) && defined(FEATURE_CORECLR) && defined(FEATURE_CORESYSTEM) && !defined(BINDER)
    if (!IsReadyToRunCompilation())
    {
        // On ARM CoreSys builds, crossgen will use 4k file alignment, as requested by Phone perf team
        // to improve perf on phones with compressed system partitions.  MDIL binder will continue to use
        // 512 byte alignment, since there is no plan to compress data partitions.
        SetFileAlignment(0x1000);
    }
#endif
}

ZapImage::CompileStatus ZapImage::CompileProfileDataWorker(mdToken token, unsigned methodProfilingDataFlags)
{
    if ((TypeFromToken(token) != mdtMethodDef) ||
        (!m_pMDImport->IsValidToken(token)))
    {
        m_zapper->Info(W("Warning: Invalid method token %08x in profile data.\n"), token);
        return NOT_COMPILED;
    }

#ifdef _DEBUG
    static ConfigDWORD g_NgenOrder;

    if ((g_NgenOrder.val(CLRConfig::INTERNAL_NgenOrder) & 2) == 2)
    {
        const ProfileDataHashEntry * foundEntry = profileDataHashTable.LookupPtr(token);
    
        if (foundEntry == NULL)
            return NOT_COMPILED;

        // The md must match.
        _ASSERTE(foundEntry->md == token); 
        // The target position cannot be 0.
        _ASSERTE(foundEntry->pos > 0);
    }
#endif

    // Now compile the method
    return TryCompileMethodDef(token, methodProfilingDataFlags);
}

void ZapImage::CompileProfileData()
{
    BeginRegion(CORINFO_REGION_HOT);

    CorProfileData* pProfileData = GetProfileData();
        
    if (m_profileDataSections[MethodProfilingData].tableSize > 0)
    {
        // record the start of hot IBC methods.
        m_iIBCMethod = m_MethodCompilationOrder.GetCount();

        //
        // Compile the hot methods in the order specified in the MethodProfilingData
        //
        for (DWORD i = 0; i < m_profileDataSections[MethodProfilingData].tableSize; i++)
        {
            unsigned methodProfilingDataFlags = m_profileDataSections[MethodProfilingData].pTable[i].flags;
            _ASSERTE(methodProfilingDataFlags != 0);

            mdToken token = m_profileDataSections[MethodProfilingData].pTable[i].token;

            if (TypeFromToken(token) == mdtMethodDef)
            {
                //
                // Compile a non-generic method
                // 
                CompileProfileDataWorker(token, methodProfilingDataFlags);
            }
            else if (TypeFromToken(token) == ibcMethodSpec)
            {
                //
                //  compile a generic/parameterized method
                // 
                CORBBTPROF_BLOB_PARAM_SIG_ENTRY *pBlobSigEntry = pProfileData->GetBlobSigEntry(token);
                
                if (pBlobSigEntry == NULL)
                {
                    m_zapper->Info(W("Warning: Did not find definition for method token %08x in profile data.\n"), token);
                }
                else // (pBlobSigEntry  != NULL)
                {
                    _ASSERTE(pBlobSigEntry->blob.token == token);

                    // decode method desc
                    CORINFO_METHOD_HANDLE pMethod = m_pPreloader->FindMethodForProfileEntry(pBlobSigEntry);
                   
                    if (pMethod)
                    {
                        m_pPreloader->AddMethodToTransitiveClosureOfInstantiations(pMethod);

                        TryCompileInstantiatedMethod(pMethod, methodProfilingDataFlags);
                    }
                }
            }
        }
        // record the start of hot Generics methods.
        m_iGenericsMethod = m_MethodCompilationOrder.GetCount();
    }

    // record the start of untrained code
    m_iUntrainedMethod = m_MethodCompilationOrder.GetCount();

    EndRegion(CORINFO_REGION_HOT);
}

#ifdef  MDIL
static COUNT_T OutputDWord(BYTE *p, DWORD d)
{
    if (p)
    {
        p[0] = (BYTE)d;
        p[1] = (BYTE)(d>>8);
        p[2] = (BYTE)(d>>16);
        p[3] = (BYTE)(d>>24);
    }
    return 4;
}

void ZapImage::UnifyGenericInstances_MDIL(ZapInfo::MDILGenericMethodDesc *pMD)
{
    // we have unified on the last arg during generation - now we do the rest
    bool change;
    do
    {
        change = false;
        for (int argToUnify = 0; argToUnify < pMD->arity; argToUnify++)
        {
            for (ZapInfo::MDILGenericMethodDesc *p = pMD; p != NULL; p = p->next)
            {
                ZapInfo::MDILGenericMethodDesc *prev = p;
                for (ZapInfo::MDILGenericMethodDesc *q = p->next; q != NULL; q = q->next)
                {
                    // we have grouped identical bodies together in the list, so if the body is
                    // not the same, we can give up - no more identical bodies will be encountered
                    if (q->mdilCodeOffs != p->mdilCodeOffs || q->debugInfoOffs != p->debugInfoOffs)
                        break;

                    // if the flavors of p and q agree except for one position, we can merge q into p
                    if (ZapInfo::ArgFlavorsMatchExcept(q->flavorSet, p->flavorSet, pMD->arity, argToUnify))
                    {
//                        GetSvcLogger()->Printf(W("merged generic bodies %08x + %08x\n"), p->flavorSet[argToUnify], q->flavorSet[argToUnify]);
                        p->flavorSet[argToUnify] |= q->flavorSet[argToUnify];

                        // delete q from the list
                        _ASSERT(prev->next == q);
                        prev->next = q->next;
                        q = prev;
                        change = true;
                    }
                    else
                    {
                        prev = q;
                    }
                }
            }
        }
    }
    while (change);
}

COUNT_T ZapImage::EncodeGenericInstance_MDIL(ZapInfo::MDILGenericMethodDesc *pMD)
{
    // count how many instances we have
    COUNT_T count = 0;
    for (ZapInfo::MDILGenericMethodDesc *p = pMD; p != NULL; p = p->next)
    {
        count++;
    }

    // compute the size to allocate in m_genericInstPool
    size_t size = sizeof(ZapInfo::MDILInstHeader) + 2*count*sizeof(DWORD) + count*pMD->arity*sizeof(ZapInfo::FlavorSet);
    size = AlignUp(size, sizeof(DWORD));

    // as usual, we put some dummy stuff at the very beginning
    if (m_genericInstPool.GetCount() == 0)
    {
        m_genericInstPool.SetCount(sizeof(DWORD));
        OutputDWord(&m_genericInstPool[0], 'MDGI');
    }
    COUNT_T genericInstOffs = m_genericInstPool.GetCount();
    m_genericInstPool.SetCount(genericInstOffs + (COUNT_T)size);

    ZapInfo::MDILInstHeader *pMIH = (ZapInfo::MDILInstHeader *)&m_genericInstPool[genericInstOffs];
    pMIH->m_arity = pMD->arity;
    pMIH->m_flags = 0;
    pMIH->m_instCount = count;

    DWORD *mdilCodeOffsets = (DWORD *)(pMIH + 1);

    ZapInfo::FlavorSet *flavorSets = (ZapInfo::FlavorSet *)(mdilCodeOffsets + 2*count);
    
    for (ZapInfo::MDILGenericMethodDesc *p = pMD; p != NULL; p = p->next)
    {
        _ASSERTE(p->mdilCodeOffs  < m_codeBuffer     [GENERIC_CODE].GetCount());
        _ASSERTE(p->debugInfoOffs < m_debugInfoBuffer[GENERIC_CODE].GetCount());

        *mdilCodeOffsets++ = p->mdilCodeOffs;
        *mdilCodeOffsets++ = p->debugInfoOffs;
        for (int i = 0; i < pMD->arity; i++)
            *flavorSets++ = p->flavorSet[i];
    }
    return genericInstOffs;
}

int ZapImage::CheckForUnmerged(ZapInfo::MDILGenericMethodDesc tab[], int last, ZapInfo::FlavorSet flavorsToMatch, WCHAR *message)
{
    int arity = tab[last].arity;
    if (flavorsToMatch == 0)
    {
        for (int i = 0; i < last; i++)
        {
            if (ZapInfo::ArgFlavorsMatchExcept(tab[last].flavorSet, tab[i].flavorSet, arity, arity))
            {
                GetSvcLogger()->Printf(W("%s"), message);
                return 1;
            }
        }
    }
    else
    {
        for (int j = 0; j < arity; j++)
        {
            for (int i = 0; i < last; i++)
            {
                if (ZapInfo::ArgFlavorsMatchExcept(tab[last].flavorSet, tab[i].flavorSet, arity, j) &&
                    tab[last].flavorSet[j] != tab[i].flavorSet[j] && (tab[last].flavorSet[j] & flavorsToMatch) && (tab[i].flavorSet[j] & flavorsToMatch))
                {
                    GetSvcLogger()->Printf(W("%s"), message);
                    return 1;
                }
            }
        }
    }
    return 0;
}

void ZapImage::EncodeGenericInstances_MDIL()
{
    // make sure m_methodRidCount and m_mapMethodRidToOffs are big enough
    COUNT_T mappingCount = m_mapGenericMethodToDesc.GetCount();
    if (m_methodRidCount < mappingCount)
        m_methodRidCount = mappingCount;
    if (m_mapMethodRidToOffs.GetCount() < mappingCount)
    {
        COUNT_T oldCount = m_mapMethodRidToOffs.GetCount();
        m_mapMethodRidToOffs.SetCount(mappingCount);
        for (COUNT_T i = oldCount; i < mappingCount; i++)
            m_mapMethodRidToOffs[i] = 0;
    }

    COUNT_T methodCount = 0;
    COUNT_T instanceCount = 0;
    COUNT_T uniqueBodyCount = 0;
    COUNT_T uniqueBodySize = 0;
    COUNT_T unmergedInstances = 0;
    COUNT_T unmergedFloatDoubleInstances = 0;
    COUNT_T unmergedSmallIntInstances = 0;
    COUNT_T unmergedIntUIntInstances = 0;
    COUNT_T unmergedIntInstances = 0;
    COUNT_T unmergedLongULongInstances = 0;
    COUNT_T unmergedFloatStructInstances = 0;
    COUNT_T unmergedLongStructInstances = 0;
    COUNT_T unmergedLongFloatInstances = 0;
    COUNT_T unmergedNullableInstances = 0;
    COUNT_T unmergedSharedStructInstances = 0;
    COUNT_T unmergedStructInstances = 0;

    for (COUNT_T i = 0; i < m_mapGenericMethodToDesc.GetCount(); i++)
    {
        ZapInfo::MDILGenericMethodDesc *pMD = m_mapGenericMethodToDesc[i];
        if (pMD == NULL)
            continue;

        methodCount++;

        UnifyGenericInstances_MDIL(pMD);

#if 0 // def _DEBUG
        DWORD prevMdilCodeOffs = 0;
        COUNT_T uniqueBodyCountForThisMethod = 0;
        COUNT_T uniqueBodySizeForThisMethod = 0;
        COUNT_T instanceCountForThisMethod = 0;
        for (ZapInfo::MDILGenericMethodDesc *p = pMD; p != NULL; p = p->next)
        {
            instanceCountForThisMethod++;
            if (prevMdilCodeOffs != p->mdilCodeOffs)
            {
                uniqueBodyCountForThisMethod++;
                uniqueBodySizeForThisMethod += p->mdilCodeSize;
                prevMdilCodeOffs = p->mdilCodeOffs;
            }
        }
        GetSvcLogger()->Printf(W("%u Instances for generic method %08x - %u unique bodies totalling %u bytes\n"),
                instanceCountForThisMethod,      TokenFromRid(i, mdtMethodDef),
                                                        uniqueBodyCountForThisMethod,
                                                                                  uniqueBodySizeForThisMethod);

        instanceCount += instanceCountForThisMethod;
        uniqueBodyCount += uniqueBodyCountForThisMethod;
        uniqueBodySize += uniqueBodySizeForThisMethod;
        const size_t MD_TABLE_SIZE = 256;
        ZapInfo::MDILGenericMethodDesc mdTab[MD_TABLE_SIZE];
        COUNT_T mdCount = 0;
        for (ZapInfo::MDILGenericMethodDesc *p = pMD; p != NULL; p = p->next)
        {
            if (mdCount < MD_TABLE_SIZE)
            {
                mdTab[mdCount] = *p;
                mdCount++;
            }
        }
        qsort(mdTab, mdCount, sizeof(mdTab[0]), ZapInfo::CmpMDILGenericMethodDesc);

        for (COUNT_T mdInx = 0; mdInx < mdCount; mdInx++)
        {
            if (mdInx >= 1 && !ZapInfo::ArgFlavorsMatchExcept(mdTab[mdInx-1].flavorSet, mdTab[mdInx].flavorSet, mdTab[mdInx].arity, mdTab[mdInx].arity-1))
                GetSvcLogger()->Printf(W("\n"));

            GetSvcLogger()->Printf(W("  %08x(%4u): "), mdTab[mdInx].mdilCodeOffs, mdTab[mdInx].mdilCodeSize);
            for (int j = 0; j < mdTab[mdInx].arity; j++)
            {
                GetSvcLogger()->Printf(W(" %08x"), mdTab[mdInx].flavorSet[j]);
            }
            unmergedInstances += CheckForUnmerged(mdTab, mdInx, 0, W(" - unmerged instance"));

            const ZapInfo::FlavorSet FLOAT_DOUBLE = (1 << ELEMENT_TYPE_R4)|(1 << ELEMENT_TYPE_R8);
            unmergedFloatDoubleInstances += CheckForUnmerged(mdTab, mdInx, FLOAT_DOUBLE, W(" - unmerged float/double instance"));

            const ZapInfo::FlavorSet SMALL_INT = (1 << ELEMENT_TYPE_BOOLEAN)|(1 << ELEMENT_TYPE_CHAR)|(1 << ELEMENT_TYPE_I1)|(1 << ELEMENT_TYPE_U1)|(1 << ELEMENT_TYPE_I2)|(1 << ELEMENT_TYPE_U2);
            unmergedSmallIntInstances += CheckForUnmerged(mdTab, mdInx, SMALL_INT, W(" - unmerged small int instance"));

            const ZapInfo::FlavorSet REGULAR_INT = (1 << ELEMENT_TYPE_I4)|(1 << ELEMENT_TYPE_U4)|(1 << ELEMENT_TYPE_I)|(1 << ELEMENT_TYPE_U);
            unmergedIntUIntInstances += CheckForUnmerged(mdTab, mdInx, REGULAR_INT, W(" - unmerged int/uint instance"));

            const ZapInfo::FlavorSet REGISTER_INT = SMALL_INT|REGULAR_INT;
            unmergedIntInstances += CheckForUnmerged(mdTab, mdInx, REGISTER_INT, W(" - unmerged int instance"));

            const ZapInfo::FlavorSet LONG_INT = (1 << ELEMENT_TYPE_I8)|(1 << ELEMENT_TYPE_U8);
            unmergedLongULongInstances += CheckForUnmerged(mdTab, mdInx, LONG_INT, W(" - unmerged long/ulong instance"));

            const ZapInfo::FlavorSet LONG_STRUCT = LONG_INT|(1 << ELEMENT_TYPE_VALUETYPE);
            unmergedLongStructInstances += CheckForUnmerged(mdTab, mdInx, LONG_STRUCT, W(" - unmerged long/struct instance"));

            const ZapInfo::FlavorSet LONG_FLOAT = LONG_INT|FLOAT_DOUBLE;
            unmergedLongFloatInstances += CheckForUnmerged(mdTab, mdInx, LONG_FLOAT, W(" - unmerged long/float instance"));

            const ZapInfo::FlavorSet FLOAT_STRUCT = FLOAT_DOUBLE|(1 << ELEMENT_TYPE_VALUETYPE);
            unmergedFloatStructInstances += CheckForUnmerged(mdTab, mdInx, FLOAT_STRUCT, W(" - unmerged float/struct instance"));

            const ZapInfo::FlavorSet NULLABLE_STRUCT = (1 << ELEMENT_TYPE_VALUETYPE)|(1 << 0x17);
            unmergedNullableInstances += CheckForUnmerged(mdTab, mdInx, NULLABLE_STRUCT, W(" - unmerged nullable instance"));

            const ZapInfo::FlavorSet SHARED_STRUCT = (1 << ELEMENT_TYPE_VALUETYPE)|(1 << 0x1e);
            unmergedSharedStructInstances += CheckForUnmerged(mdTab, mdInx, SHARED_STRUCT, W(" - unmerged shared struct instance"));

            const ZapInfo::FlavorSet STRUCT = (1 << ELEMENT_TYPE_VALUETYPE)|(1 << 0x17)|(1 << 0x1e)|(1 << 0x1f);
            unmergedStructInstances += CheckForUnmerged(mdTab, mdInx, STRUCT, W(" - unmerged struct instance"));

            GetSvcLogger()->Printf(W("\n"));
        }
#endif
        COUNT_T genericInstOffs = EncodeGenericInstance_MDIL(pMD);

        _ASSERT(m_mapMethodRidToOffs[i] == 0);
        m_mapMethodRidToOffs[i] = GENERIC_METHOD_REF | genericInstOffs;
    }

#if 0 //def _DEBUG
    for (COUNT_T i = 0; i < m_mapGenericMethodToDesc.GetCount(); i++)
    {
        ZapInfo::MDILGenericMethodDesc *pMD = m_mapGenericMethodToDesc[i];
        if (pMD == NULL)
            continue;

        // 0 the mdilCodeOffs and unify - the result tells us what we have covered...
        for (ZapInfo::MDILGenericMethodDesc *p = pMD; p != NULL; p = p->next)
            p->mdilCodeOffs = 0;

        UnifyGenericInstances_MDIL(pMD);

        GetSvcLogger()->Printf(W("Instances for generic method %08x\n"), TokenFromRid(i, mdtMethodDef));

        const size_t MD_TABLE_SIZE = 256;
        ZapInfo::MDILGenericMethodDesc mdTab[MD_TABLE_SIZE];
        COUNT_T mdCount = 0;
        for (ZapInfo::MDILGenericMethodDesc *p = pMD; p != NULL; p = p->next)
        {
            if (mdCount < MD_TABLE_SIZE)
            {
                mdTab[mdCount] = *p;
                mdCount++;
            }
        }
        qsort(mdTab, mdCount, sizeof(mdTab[0]), ZapInfo::CmpMDILGenericMethodDesc);

        for (COUNT_T mdInx = 0; mdInx < mdCount; mdInx++)
        {
            if (mdInx >= 1 && !ZapInfo::ArgFlavorsMatchExcept(mdTab[mdInx-1].flavorSet, mdTab[mdInx].flavorSet, mdTab[mdInx].arity, mdTab[mdInx].arity-1))
                GetSvcLogger()->Printf(W("\n"));

            for (int j = 0; j < mdTab[mdInx].arity; j++)
            {
                GetSvcLogger()->Printf(W(" %08x"), mdTab[mdInx].flavorSet[j]);
            }

            GetSvcLogger()->Printf(W("\n"));
        }
    }

    GetSvcLogger()->Printf(W("%u instances and %u unique bodies for %u generic methods\n"), instanceCount, uniqueBodyCount, methodCount);
    GetSvcLogger()->Printf(W("%u unmerged instances\n"), unmergedInstances);
    GetSvcLogger()->Printf(W("%u unmerged float/double instances\n"), unmergedFloatDoubleInstances);
    GetSvcLogger()->Printf(W("%u unmerged small int instances\n"), unmergedSmallIntInstances);
    GetSvcLogger()->Printf(W("%u unmerged int/uint instances\n"), unmergedIntUIntInstances);
    GetSvcLogger()->Printf(W("%u unmerged int instances\n"), unmergedIntInstances);
    GetSvcLogger()->Printf(W("%u unmerged long/ulong instances\n"), unmergedLongULongInstances);
    GetSvcLogger()->Printf(W("%u unmerged long/struct instances\n"), unmergedLongStructInstances);
    GetSvcLogger()->Printf(W("%u unmerged long/float instances\n"), unmergedLongFloatInstances);
    GetSvcLogger()->Printf(W("%u unmerged float/struct instances\n"), unmergedFloatStructInstances);
    GetSvcLogger()->Printf(W("%u unmerged nullable instances\n"), unmergedNullableInstances);
    GetSvcLogger()->Printf(W("%u unmerged shared struct instances\n"), unmergedSharedStructInstances);
    GetSvcLogger()->Printf(W("%u unmerged struct instances\n"), unmergedStructInstances);

    GetSvcLogger()->Printf(W("%u unique generic body size\n"), uniqueBodySize);

    GetSvcLogger()->Printf(W("%u unmerged generic methods\n"), m_unmergedGenericCount);
    GetSvcLogger()->Printf(W("%u   merged generic methods\n"), m_mergedGenericCount);
    GetSvcLogger()->Printf(W("%u unmerged generic code size\n"), m_unmergedGenericSize);
    GetSvcLogger()->Printf(W("%u   merged generic code size\n"), m_mergedGenericSize);
#endif
}



//----------------------------------------------------------------------------------
// Copies the specified number of bytes from fpIn to fpOut.
//----------------------------------------------------------------------------------
static bool fcopy(FILE *fpIn, FILE *fpOut, size_t cbBytes)
{
    size_t cbNumBytesLeft = cbBytes;

    while (cbNumBytesLeft)
    {
        byte buffer[PAGE_SIZE];
        size_t cbNumBytesForThisPass = min(cbNumBytesLeft, sizeof(buffer));
        if (1 != fread(buffer, cbNumBytesForThisPass, 1, fpIn))
            return false;
        if (1 != fwrite(buffer, cbNumBytesForThisPass, 1, fpOut))
            return false;
        cbNumBytesLeft -= cbNumBytesForThisPass;
    }
    return true;
}


//----------------------------------------------------------------------------------
// Writes the specified number of bytes at a specific position in the output file.
//----------------------------------------------------------------------------------
static bool fwriteat(FILE *fpOut, ULONG position, const void *pBytes, size_t cbBytes)
{
    if (0 != fseek(fpOut, position, SEEK_SET))
        return false;
    if (1 != fwrite(pBytes, cbBytes, 1, fpOut))
        return false;
    return true;
}

//----------------------------------------------------------------------------------
// Writes out zeroes to "fp" until the file position is a multiple of "align".
//----------------------------------------------------------------------------------
static bool fzerofilluntilaligned(LONG align, FILE *fp)
{
    LONG pos = ftell(fp);
    LONG endpoint = (LONG)ALIGN_UP(pos, align);
    for (LONG i = pos; i < endpoint; i++)
    {
        BYTE zero = 0;
        if (1 != fwrite(&zero, 1, 1, fp))
            return false;
    }
    return true;
}


//----------------------------------------------------------------------------------
// When we insert the .MDIL section, we insert bytes into two portions of the IL image.
//
// - Insertion point #1 starts at the end of the original section table (we need a new
//   entry for the .MDIL section.) In practice, this always pushes the section table
//   into a new FileAlignment page and thus requires bumping everything below
//   by other (FileAlignment - sizeof(IMAGE_SECTION_HEADER)) bytes to preserve alignment.
//
//   For simplicity, we do this whether or not the section table actually spilled over.
//
//
// - Insertion point #2 starts after the last original section contents. We insert
//   the contents of the .MDIL section here.
//
// The bytes in between the insertion points are blitted to the output file
// (except for a few needed fixups.)
//
// It was also attempted to reduce the number of insertion points to 1 by
// inserting the .MDIL contents before the other sections. But PEDecoder boots
// any PE whose section table isn't sorted by both RawData and RVA addresses so
// this pulled the cord on that idea.
//----------------------------------------------------------------------------------
enum FIXUPREGIONID
{
    FIXUPREGIONID_SECTIONCONTENTS = 0,  // region from end of original section table to end of final original section contents.
    FIXUPREGIONID_CERTIFICATES    = 1,  // region from end of section contents to end of file (WIN_CERTIFICATE stuff goes here.)
    FIXUPREGIONID_COUNT           = 2,

};


//----------------------------------------------------------------------------------
// We create an array of these, sorted by m_start. The array is terminated by
// an entry whose m_start is the size of the input file.
//----------------------------------------------------------------------------------
struct FixupRegion
{
    ULONG m_start;    // Position of first byte of region (in the input file)
    ULONG m_delta;    // Amount to add to make it correct for the output file.
};

static DWORD FixupPosition(const FixupRegion *pFixupRegions, ULONG inputPosition, ULONG *pOutputPosition)
{
    ULONG delta = 0;
    while (inputPosition >= pFixupRegions->m_start)
    {
        delta = pFixupRegions->m_delta;
        if (delta == ((ULONG)(-1)))
            return ERROR_BAD_FORMAT;  // A FilePointer read from the input file is out of range.

        pFixupRegions++;
    }
    *pOutputPosition = inputPosition + delta;
    return ERROR_SUCCESS;
}


//----------------------------------------------------------------------------------
// Creates a copy of the input IL file with a new ".mdil" section attached.
//----------------------------------------------------------------------------------
static DWORD EmbedMdilIntoILFile(FILE *inputFile, FILE *outputFile, LPCWSTR inputFileName, ZapImage *pZapImage)
{
#ifdef BINDER
    _ASSERTE(!"intentionally unreachable");
    return E_NOTIMPL;
#else

    _ASSERTE(0 == ftell(inputFile));
    _ASSERTE(0 == ftell(outputFile));


    static const BYTE aMDILSectionName[IMAGE_SIZEOF_SHORT_NAME] = {'.','m','d','i','l',0,0,0};

    NewHolder<IMAGE_SECTION_HEADER> oldImageSectionHeaders;

    //-----------------------------------------------------------------------------------------
    // Read the PE headers.
    //-----------------------------------------------------------------------------------------
    IMAGE_DOS_HEADER dosHeader;
    if (fread(&dosHeader, sizeof(dosHeader), 1, inputFile) != 1) goto ioerror;
    if (dosHeader.e_magic != IMAGE_DOS_SIGNATURE)
    {
        pZapImage->GetZapper()->Error(W("Error: \"%ws\": Expected 'MZ' at offset 0.\n"), inputFileName);
        goto error;  // No 'MZ'
    }

    size_t cbPEOffset = dosHeader.e_lfanew;
    if (0 != fseek(inputFile, cbPEOffset, SEEK_SET)) goto ioerror;
    DWORD peSignature;
    if (1 != fread(&peSignature, sizeof(peSignature), 1, inputFile)) goto ioerror;
    if (peSignature != IMAGE_NT_SIGNATURE) 
    {
        pZapImage->GetZapper()->Error(W("Error: \"%ws\": Expected 'PE\\0\\0' at offset 0x%x.\n"), inputFileName, ftell(inputFile) - sizeof(peSignature));
        goto error; // No 'PE\0\0'
    }

    ULONG positionOfImageFileHeader = ftell(inputFile);
    IMAGE_FILE_HEADER imageFileHeader;
    if (1 != fread(&imageFileHeader, sizeof(imageFileHeader), 1, inputFile)) goto ioerror;
    const int numberOfSections = imageFileHeader.NumberOfSections;

    if (numberOfSections <= 0 || numberOfSections > 2048)  // crude buffer overflow guard
    {
        pZapImage->GetZapper()->Error(W("Error: \"%ws\": Suspicious value for IMAGE_FILE_HEADER.NumberOfSections: %d.\n"), inputFileName, numberOfSections);
        goto error; // No 'PE\0\0'
    }

    ULONG positionOfImageOptionalHeader = ftell(inputFile);
    IMAGE_OPTIONAL_HEADER32 imageOptionalHeader;
    if (1 != fread(&imageOptionalHeader, sizeof(imageOptionalHeader), 1, inputFile)) goto error;
    if (imageOptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) //0x10b
    {
        // No 0x10b magic. Thus, not a 32-bit header. (If you saw 0x20b here, this is a PE with a 64-bit header.)
        if (imageOptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        {
            pZapImage->GetZapper()->Error(W("Error: \"%ws\": This is a 64-bit image.\n"), inputFileName);
        }
        else
        {
            pZapImage->GetZapper()->Error(W("Error: \"%ws\": Unexpected IMAGE_OPTIONAL_HEADER.Magic value: 0x%x.\n"),
                                          inputFileName,
                                          (unsigned int)(imageOptionalHeader.Magic));        }

        goto error;
    }

    if (imageOptionalHeader.NumberOfRvaAndSizes != IMAGE_NUMBEROF_DIRECTORY_ENTRIES)
    {
        // Expected 16 IMAGE_DATA_DIRECTORY entries (an assumption hard-coded into the struct definition of IMAGE_OPTIONAL_HEADER32)
        pZapImage->GetZapper()->Error(W("Error: \"%ws\": Unexpected IMAGE_OPTIONAL_HEADER.NumberOfRvaAndSizes value: 0x%x.\n"),
                                      inputFileName,
                                      (unsigned int)(imageOptionalHeader.NumberOfRvaAndSizes));
        goto error;   
    }

    
    //-----------------------------------------------------------------------------------------
    // Read the IMAGE_SECTION_HEADER array.
    //-----------------------------------------------------------------------------------------
    if (NULL == (oldImageSectionHeaders = new (nothrow) IMAGE_SECTION_HEADER[numberOfSections])) goto oomerror;
    size_t rvaForNewSection = 0;
    int sectionIndexOfPreexistingMidlSection = -1;
    ULONG endOfLastOriginalPhysicalSector = 0;
    ULONG positionOfOriginalSectionTable = ftell(inputFile);
    for (int sidx = 0; sidx < numberOfSections; sidx++)
    {
        ULONG positionOfSectionHeader = ftell(inputFile);
        if (1 != fread(&oldImageSectionHeaders[sidx], sizeof(IMAGE_SECTION_HEADER), 1, inputFile)) goto ioerror;
        if (0 == memcmp(aMDILSectionName, oldImageSectionHeaders[sidx].Name, IMAGE_SIZEOF_SHORT_NAME))
        {
            // If we are asked to generate MDIL, but the current file already has MDIL section,
            // we change the section name, and then put in new MDIL section.  The old MDIL section will not
            // be put into final ni image.
            // This is support phone build which puts IL with MDIL on device.
            sectionIndexOfPreexistingMidlSection = sidx;
        }

        // Pointer and Size of RawData must be aligned.
        if (0 != oldImageSectionHeaders[sidx].PointerToRawData % imageOptionalHeader.FileAlignment)
        {
            pZapImage->GetZapper()->Error(W("Error: \"%ws\": Section #%d: PointerToRawData not aligned with IMAGE_OPTIONAL_HEADER.FileAlignment.\n"), inputFileName, (sidx + 1));
            goto error;
        }
        if (0 != oldImageSectionHeaders[sidx].SizeOfRawData % imageOptionalHeader.FileAlignment)
        {
            pZapImage->GetZapper()->Error(W("Error: \"%ws\": Section #%d: SizeOfRawData not aligned with IMAGE_OPTIONAL_HEADER.FileAlignment.\n"), inputFileName, (sidx + 1));
            goto error;
        }

        endOfLastOriginalPhysicalSector = max(endOfLastOriginalPhysicalSector, oldImageSectionHeaders[sidx].PointerToRawData + oldImageSectionHeaders[sidx].SizeOfRawData);

        size_t spaceNeededForThisSection = ALIGN_UP(oldImageSectionHeaders[sidx].Misc.VirtualSize, imageOptionalHeader.SectionAlignment);
        size_t nextFreeRva = oldImageSectionHeaders[sidx].VirtualAddress + spaceNeededForThisSection;
        if (nextFreeRva > rvaForNewSection)
            rvaForNewSection = nextFreeRva;
    }
    ULONG positionOfFirstByteAfterOriginalSectionTable = ftell(inputFile);

    //-----------------------------------------------------------------------------------------
    // Block copy everything to the end of the original section table.
    //-----------------------------------------------------------------------------------------
    if (0 != fseek(inputFile, 0, SEEK_SET)) goto ioerror;
    if (!fcopy(inputFile, outputFile, positionOfFirstByteAfterOriginalSectionTable)) goto ioerror;

    //-----------------------------------------------------------------------------------------
    // Write out the new .mdil section header. (It is not quite filled out yet so this
    // is simply the easiest way to advance the file pointer.)
    //-----------------------------------------------------------------------------------------
    IMAGE_SECTION_HEADER mdilSectionHeader;
    memset(&mdilSectionHeader, 0, sizeof(mdilSectionHeader));
    memcpy(mdilSectionHeader.Name, aMDILSectionName, IMAGE_SIZEOF_SHORT_NAME);
    mdilSectionHeader.VirtualAddress = rvaForNewSection;
    mdilSectionHeader.SizeOfRawData = 0xcccccccc; // Will need fixup later
    mdilSectionHeader.PointerToRawData = 0xcccccccc; // Will need fixup later
    mdilSectionHeader.Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;

    ULONG outputPositionOfMdilSectionHeader = ftell(outputFile);
    if (1 != fwrite(&mdilSectionHeader, sizeof(mdilSectionHeader), 1, outputFile)) goto ioerror;

    //-----------------------------------------------------------------------------------------
    // Adding the extra section header can (and usually does) cause the section table to spill
    // over into a new FileAlignment page. In such a case, we have to bump all the section contents
    // by FileAlignment bytes.
    //
    // For simplicity (and since C# always ends up on this case anyway), always bump even if not
    // necessary.
    //-----------------------------------------------------------------------------------------
    for (ULONG i = 0; i < imageOptionalHeader.FileAlignment - sizeof(IMAGE_SECTION_HEADER); i++)
    {
        BYTE zero = 0;
        if (1 != fwrite(&zero, sizeof(zero), 1, outputFile)) goto ioerror;
    }

    //-----------------------------------------------------------------------------------------
    // Block copy everything from the end of the original section table to the end of the section contents.
    //-----------------------------------------------------------------------------------------
    ULONG sizeOfOriginalSectionContents = endOfLastOriginalPhysicalSector - positionOfFirstByteAfterOriginalSectionTable;
    if (0 != fseek(inputFile, positionOfFirstByteAfterOriginalSectionTable, SEEK_SET)) goto error;
    if (!fcopy(inputFile, outputFile, sizeOfOriginalSectionContents)) goto ioerror;

    
    //-----------------------------------------------------------------------------------------
    // Write out the actual MDIL
    //-----------------------------------------------------------------------------------------
    mdilSectionHeader.PointerToRawData = ftell(outputFile);
    // Our previous alignment checks on the section's PointerToRawData and SizeOfRawData should guarantee this assert
    _ASSERTE(0 == (mdilSectionHeader.PointerToRawData % imageOptionalHeader.FileAlignment));  
    DWORD errorCode = pZapImage->Write_MDIL(outputFile);
    if (errorCode != ERROR_SUCCESS)
        return errorCode;

    //-----------------------------------------------------------------------------------------
    // Add pad bytes after the MDIL to satisfy the section alignment requirement.
    //-----------------------------------------------------------------------------------------
    mdilSectionHeader.Misc.VirtualSize = ftell(outputFile) - mdilSectionHeader.PointerToRawData;
    mdilSectionHeader.SizeOfRawData = (DWORD)ALIGN_UP(mdilSectionHeader.Misc.VirtualSize, imageOptionalHeader.FileAlignment);
    if (!fzerofilluntilaligned(imageOptionalHeader.FileAlignment, outputFile)) goto ioerror;

    //-----------------------------------------------------------------------------------------
    // Copy out any stuff after the section contents (e.g. WIN_CERTIFICATE)
    //-----------------------------------------------------------------------------------------
    if (0 != fseek(inputFile, 0, SEEK_END)) goto ioerror;
    ULONG inputFileSize = ftell(inputFile);
    ULONG sizeOfStuffAfterSectionContents = inputFileSize - endOfLastOriginalPhysicalSector;
    if (0 != fseek(inputFile, endOfLastOriginalPhysicalSector, SEEK_SET)) goto ioerror;
    if (!fcopy(inputFile, outputFile, sizeOfStuffAfterSectionContents)) goto ioerror;
    ULONG outputFileSize = ftell(outputFile);


    //=========================================================================================
    // End of pass 1. Now do fixups.
    //=========================================================================================

    //-----------------------------------------------------------------------------------------
    // Record the various regions and their fixup data for easy lookup.
    //-----------------------------------------------------------------------------------------
    FixupRegion aFixupRegions[FIXUPREGIONID_COUNT + 1];
    memset(&aFixupRegions, 0xcc, sizeof(aFixupRegions));

    aFixupRegions[FIXUPREGIONID_SECTIONCONTENTS].m_start = positionOfFirstByteAfterOriginalSectionTable;
    aFixupRegions[FIXUPREGIONID_SECTIONCONTENTS].m_delta = imageOptionalHeader.FileAlignment; 


    aFixupRegions[FIXUPREGIONID_CERTIFICATES].m_start = endOfLastOriginalPhysicalSector;
    aFixupRegions[FIXUPREGIONID_CERTIFICATES].m_delta = outputFileSize - inputFileSize; 

    aFixupRegions[FIXUPREGIONID_COUNT].m_start = inputFileSize;
    aFixupRegions[FIXUPREGIONID_COUNT].m_delta = (ULONG)(-1);


    //-----------------------------------------------------------------------------------------
    // IMAGE_FILE_HEADER.NumberOfSections is one bigger. Duh.
    //-----------------------------------------------------------------------------------------
    WORD newNumberOfSections = imageFileHeader.NumberOfSections + 1;
    if (!fwriteat(outputFile,
                  positionOfImageFileHeader + offsetof(IMAGE_FILE_HEADER, NumberOfSections),
                  &newNumberOfSections,
                  sizeof(newNumberOfSections)))
        goto ioerror;

    //-----------------------------------------------------------------------------------------
    // We added a new .MDIL section so add its size to IMAGE_OPTIONAL_HEADER.SizeOfInitializedData.
    //-----------------------------------------------------------------------------------------
    DWORD newSizeOfInitializedData = imageOptionalHeader.SizeOfInitializedData + mdilSectionHeader.SizeOfRawData;
    if (!fwriteat(outputFile,
                  positionOfImageOptionalHeader + offsetof(IMAGE_OPTIONAL_HEADER, SizeOfInitializedData),
                  &newSizeOfInitializedData,
                  sizeof(newSizeOfInitializedData)))
        goto ioerror;

    if (0 != (imageOptionalHeader.SizeOfImage % imageOptionalHeader.SectionAlignment))
    {
        pZapImage->GetZapper()->Error(W("Error: \"%ws\": IMAGE_OPTIONAL_HEADER.SizeOfImage not aligned with IMAGE_OPTIONAL_HEADER.SectionAlignment.\n"), inputFileName);
        goto error;   // Incoming PE format violation: SizeOfImage not a multple of SectionAlignment
    }

    //-----------------------------------------------------------------------------------------
    // We added a new .MDIL section so add its in-memory size requirements to IMAGE_OPTIONAL_HEADER.SizeOfImage.
    //-----------------------------------------------------------------------------------------
    DWORD newSizeOfImage = imageOptionalHeader.SizeOfImage + (DWORD)ALIGN_UP(mdilSectionHeader.Misc.VirtualSize, imageOptionalHeader.SectionAlignment);
    if (!fwriteat(outputFile,
                  positionOfImageOptionalHeader + offsetof(IMAGE_OPTIONAL_HEADER, SizeOfImage),
                  &newSizeOfImage,
                  sizeof(newSizeOfImage)))
        goto ioerror;

    //-----------------------------------------------------------------------------------------
    // We added a new IMAGE_SECTION_HEADER so recompute IMAGE_OPTIONAL_HEADER.SizeOfHeaders
    //-----------------------------------------------------------------------------------------
    ULONG newSizeOfHeaders = (ULONG)(ALIGN_UP(outputPositionOfMdilSectionHeader + sizeof(IMAGE_SECTION_HEADER), imageOptionalHeader.FileAlignment));

    if (newSizeOfHeaders > oldImageSectionHeaders[0].VirtualAddress)
    {
        // A corner case that can only come up if the input file has a ridiculously low SectionAlignment (512 bytes) or
        // a ridiculous number of sections (50).
        pZapImage->GetZapper()->Error(
            W("Tool limitation: \"%ws\": Could not embed MDIL into image as there is not enough room to grow the section header table without ")
            W("modifying the section RVAs. Modifying section RVAs is not supported by this tool. It may be possible to avoid this ")
            W("by rebuilding the input image with a smaller FileAlignment or a larger SectionAlignment. We are sorry for the inconvenience.\n"),
            inputFileName);
        goto error;
    }

    if (!fwriteat(outputFile,
                  positionOfImageOptionalHeader + offsetof(IMAGE_OPTIONAL_HEADER, SizeOfHeaders),
                  &newSizeOfHeaders,
                  sizeof(newSizeOfHeaders)))
        goto ioerror;

    //-----------------------------------------------------------------------------------------
    // We bumped the section contents by FileAlignment so add that to the original section headers PointerToRawData values.
    //-----------------------------------------------------------------------------------------
    for (int sidx = 0; sidx < imageFileHeader.NumberOfSections; sidx++)
    {
        DWORD newPointerToRawData = oldImageSectionHeaders[sidx].PointerToRawData + aFixupRegions[FIXUPREGIONID_SECTIONCONTENTS].m_delta;
        if (!fwriteat(outputFile,
                      positionOfOriginalSectionTable + sidx * sizeof(IMAGE_SECTION_HEADER) + offsetof(IMAGE_SECTION_HEADER, PointerToRawData),
                      &newPointerToRawData,
                      sizeof(newPointerToRawData)))

            goto ioerror;
    }


    //-----------------------------------------------------------------------------------------
    // We've now fully filled in the .MDIL section header. Rewrite it.
    //-----------------------------------------------------------------------------------------
    if (!fwriteat(outputFile,
                  outputPositionOfMdilSectionHeader,
                  &mdilSectionHeader,
                  sizeof(mdilSectionHeader)))
        goto ioerror;

    //-----------------------------------------------------------------------------------------
    // Some joker gave us an input with a .MDIL section already in it. Rename it
    // and the binder will drop it over the side.
    //-----------------------------------------------------------------------------------------
    if (sectionIndexOfPreexistingMidlSection != -1)
    {
        BYTE nameMangler = '0' + sectionIndexOfPreexistingMidlSection;
        if (!fwriteat(outputFile,
                      positionOfOriginalSectionTable +
                      sectionIndexOfPreexistingMidlSection * sizeof(IMAGE_SECTION_HEADER)
                      + offsetof(IMAGE_SECTION_HEADER, Name)
                      + 4,
                      &nameMangler,
                      sizeof(nameMangler)))
            goto ioerror;
    }


    //-----------------------------------------------------------------------------------------
    // IMAGE_FILE_HEADER.PointerToSymbolTable is always supposed to be 0 for managed PE's.
    // If you remove this restriction, you'll need to add fixup code.
    //-----------------------------------------------------------------------------------------
    if (imageFileHeader.PointerToSymbolTable != 0)
    {
        pZapImage->GetZapper()->Error(W("Error: \"%ws\": IMAGE_FILE_HEADER.PointerToSymbolTable expected to be 0.\n"), inputFileName);
        goto error;
    }

    //-----------------------------------------------------------------------------------------
    // IMAGE_DEBUG_DIRECTORY if present has an absolute file pointer to RSDS structure. Fix it up.
    //-----------------------------------------------------------------------------------------
    ULONG rvaOfOldImageDebugDirectory = imageOptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].VirtualAddress;
    if (rvaOfOldImageDebugDirectory != 0)
    {
        if (0 != (imageOptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].Size % sizeof(IMAGE_DEBUG_DIRECTORY)))
        {
            // Yes, we have real MP apps that trigger this...
            pZapImage->GetZapper()->Warning(W("Warning: \"%ws\": DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].Size expected to be a multiple of %d.\n"), inputFileName, sizeof(IMAGE_DEBUG_DIRECTORY));
        }
        else
        {
            int sidx;
            for (sidx = 0; sidx < numberOfSections; sidx++)
            {
                if (rvaOfOldImageDebugDirectory >= oldImageSectionHeaders[sidx].VirtualAddress &&
                    rvaOfOldImageDebugDirectory < oldImageSectionHeaders[sidx].VirtualAddress + oldImageSectionHeaders[sidx].Misc.VirtualSize)
                {
                    ULONG positionOfOldImageDebugDirectory =
                                oldImageSectionHeaders[sidx].PointerToRawData +
                                rvaOfOldImageDebugDirectory -
                                oldImageSectionHeaders[sidx].VirtualAddress;
    
                    ULONG positionOfNewImageDebugDirectory;
                    DWORD errorResult = FixupPosition(aFixupRegions, positionOfOldImageDebugDirectory, &positionOfNewImageDebugDirectory);
                    if (errorResult != ERROR_SUCCESS)
                        goto error;
    
                    DWORD numImageDebugDirectories = imageOptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].Size / sizeof(IMAGE_DEBUG_DIRECTORY); 
                    for (DWORD i = 0; i < numImageDebugDirectories; i++)
                    {
                        if (0 != fseek(inputFile, positionOfOldImageDebugDirectory, SEEK_SET)) goto ioerror;
                        IMAGE_DEBUG_DIRECTORY imageDebugDirectory;
                        if (1 != fread(&imageDebugDirectory, sizeof(imageDebugDirectory), 1, inputFile)) goto ioerror;
        
                        ULONG positionOfNewDebugRawData = 0xcccccccc;
                        errorResult = FixupPosition(aFixupRegions, imageDebugDirectory.PointerToRawData, &positionOfNewDebugRawData);
                        if (errorResult != ERROR_SUCCESS)
                        {
                            if (errorResult != ERROR_BAD_FORMAT)
                                goto error;
    
                            // Don't make this a fatal error: not everyone sets IMAGE_DEBUG_DIRECTORY.PointerToRawData correctly.
                            pZapImage->GetZapper()->Warning(W("Warning: \"%ws\": IMAGE_DEBUG_DIRECTORY.PointerToRawData has an out of range value: 0x%x.\n"), inputFileName, imageDebugDirectory.PointerToRawData);
                        }
                        else
                        {
                            if (!fwriteat(outputFile, positionOfNewImageDebugDirectory + offsetof(IMAGE_DEBUG_DIRECTORY, PointerToRawData), &positionOfNewDebugRawData, sizeof(positionOfNewDebugRawData)))
                                goto error;
                        }
    
                        positionOfOldImageDebugDirectory += sizeof(IMAGE_DEBUG_DIRECTORY);
                        positionOfNewImageDebugDirectory += sizeof(IMAGE_DEBUG_DIRECTORY);
                    }
                    break;
                }
            }
            if (sidx == numberOfSections)
            {
                pZapImage->GetZapper()->Error(W("Error: \"%ws\": DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].VirtualAddress points outside the bounds of the image: 0x%x.\n"), inputFileName, rvaOfOldImageDebugDirectory);
                goto error;  // Could not resolve IMAGE_DEBUG_DIRECTORY rva.
            }
        }
    }


    //-----------------------------------------------------------------------------------------
    // The WIN_CERTIFICATE structure, if present, is stored at the end of the PE file outside of
    // any section. The so-called "rva" at IMAGE_DATA_DIRECTORY[4] is actually an absolute file position.
    //-----------------------------------------------------------------------------------------
    ULONG oldPositionOfWinCertificate = imageOptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY].VirtualAddress;
    if (oldPositionOfWinCertificate != 0)
    {
        ULONG newPositionOfWinCertificate;
        DWORD errorCode = FixupPosition(aFixupRegions, oldPositionOfWinCertificate, &newPositionOfWinCertificate);
        if (errorCode != ERROR_SUCCESS)
        {
            pZapImage->GetZapper()->Error(W("Error: \"%ws\": DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY].VirtualAddress points outside the bounds of the image: 0x%x.\n"), inputFileName, oldPositionOfWinCertificate);
            goto error;
        }

        if (!fwriteat(outputFile,
                      positionOfImageOptionalHeader
                      + offsetof(IMAGE_OPTIONAL_HEADER, DataDirectory)
                      + sizeof(IMAGE_DATA_DIRECTORY) * IMAGE_DIRECTORY_ENTRY_SECURITY
                      + offsetof(IMAGE_DATA_DIRECTORY, VirtualAddress),
                      &newPositionOfWinCertificate,
                      sizeof(newPositionOfWinCertificate)))
        {
            goto ioerror;
        }
    }
    
    //-----------------------------------------------------------------------------------------
    // Force NX_COMPAT and DYNAMIC_BASE so secure OS loaders can load the image (obfuscators 
    // tend to strip these off)
    //-----------------------------------------------------------------------------------------
    DWORD newDllCharacteristics = imageOptionalHeader.DllCharacteristics | IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE | IMAGE_DLLCHARACTERISTICS_NX_COMPAT;
    if (!fwriteat(outputFile,
                  positionOfImageOptionalHeader + offsetof(IMAGE_OPTIONAL_HEADER, DllCharacteristics),
                  &newDllCharacteristics,
                  sizeof(newDllCharacteristics)))
        goto error;

    //=========================================================================================
    // End of final pass. Output complete.
    //=========================================================================================

    return ERROR_SUCCESS;

ioerror:
    pZapImage->GetZapper()->Error(W("Error: \"%ws\": Unexpected end of file.\n"), inputFileName);
    return E_FAIL;

oomerror:
    return E_OUTOFMEMORY;

error:
    return ERROR_BAD_FORMAT;
#endif // BINDER
}



void ZapImage::Output_MDIL()
{
#ifdef BINDER
    _ASSERTE(!"intentionally unreachable");
#else


    StackSString outputFileName(m_zapper->GetOutputFileName());
    FILE *outputFile = _wfopen(outputFileName.GetUnicode(), W("wb"));
    if (outputFile == NULL)
        return;

    FILE *inputFile = _wfopen(m_pModuleFileName, W("rb"));
    if (!inputFile) goto error;

    DWORD errorCode = EmbedMdilIntoILFile(inputFile, outputFile, m_pModuleFileName, this);
    if (errorCode != ERROR_SUCCESS)
    {
        SetLastError(errorCode);
        goto error;
    }

    fclose(inputFile);
    fclose(outputFile);
    
    //    GetSvcLogger()->Printf(W("finished generating %s file\n"), outputFileName.GetUnicode());
    return;
    

error:
    DWORD dwLastError = GetLastError();
    fclose(inputFile);
    fclose(outputFile);
    WszDeleteFile(outputFileName.GetUnicode());
    m_zapper->Error(W("Could not create %ls file\n"), outputFileName.GetUnicode());
    SetLastError(dwLastError);
    ThrowLastError();
#endif // BINDER
}


//----------------------------------------------------------------------------------
// Writes out the MDIL blob.
//----------------------------------------------------------------------------------
DWORD ZapImage::Write_MDIL(FILE *outputFile)
{
#ifdef BINDER
    _ASSERTE(!"intentionally unreachable");
    return E_NOTIMPL;
#else

    if (m_pICLW != NULL)
    {
        delete m_pICLW;
        m_pICLW = NULL;
    }
    MDILHeader mdilHeader;
    memset(&mdilHeader, 0, sizeof(mdilHeader));

    if (m_methodRidCount == 0)
    {
        m_mapMethodRidToOffs.SetCount(1);
		m_mapMethodRidToOffs[0] = 0xcafedead;
        m_methodRidCount = 1;
    }

    DWORD totalCodeSize = 0;
    DWORD totalDebugInfoSize = 0;
    for (int codeKind = GENERIC_CODE; codeKind < CODE_KIND_COUNT; codeKind++)
    {
        if (m_codeOffs[codeKind] < sizeof(DWORD) && codeKind == GENERIC_CODE)
        {
            _ASSERTE(m_codeOffs[codeKind] == 0);
            m_codeBuffer[codeKind].SetCount(sizeof(DWORD));
            m_codeOffs[codeKind] = sizeof(DWORD);

            OutputDWord(&m_codeBuffer[codeKind][0], 'MDCD');

        }
        totalCodeSize += m_codeOffs[codeKind];
        totalDebugInfoSize += m_debugInfoBuffer[codeKind].GetCount();
    }

    EncodeGenericInstances_MDIL();

    // turns out we actually need an exact method count
    IMDInternalImport * pMDImport = m_pMDImport;
    HENUMInternalHolder hEnum(pMDImport);
    hEnum.EnumAllInit(mdtMethodDef);
    m_methodRidCount = hEnum.EnumGetCount() + m_stubMethodCount + 1;
    if (m_methodRidCount  < m_mapMethodRidToOffs.GetCount())
    {
        for (COUNT_T i = m_methodRidCount; i < m_mapMethodRidToOffs.GetCount(); i++)
            _ASSERTE(m_mapMethodRidToOffs[i] == 0);
    }
    else if (m_mapMethodRidToOffs.GetCount() < m_methodRidCount)
    {
        COUNT_T oldCount = m_mapMethodRidToOffs.GetCount();
        m_mapMethodRidToOffs.SetCount(m_methodRidCount);
        for (COUNT_T i = oldCount; i < m_methodRidCount; i++)
            m_mapMethodRidToOffs[i] = 0;
    }


    // conceptually, the code buffers for generic and non-generic code should be treated as one buffer
    // that implies that we need to add the size of the generic code buffer to offsets in the non-generic code
    // buffer
    for (COUNT_T methodRid = 0; methodRid < m_mapMethodRidToOffs.GetCount(); methodRid++)
    {
        if ((m_mapMethodRidToOffs[methodRid] != 0) &&
            ((m_mapMethodRidToOffs[methodRid] & GENERIC_METHOD_REF) == 0))
        {
            m_mapMethodRidToOffs[methodRid] += m_codeOffs[GENERIC_CODE];
        }
    }

    for (COUNT_T methodRid = 0; methodRid < m_mapMethodRidToDebug.GetCount(); methodRid++)
    {
        if (m_mapMethodRidToDebug[methodRid] != 0xFFFFFFFF)
            m_mapMethodRidToDebug[methodRid] += m_debugInfoBuffer[GENERIC_CODE].GetCount();
        else
            m_mapMethodRidToDebug[methodRid] = 0;
    }


    mdilHeader.hdrSize              = sizeof(mdilHeader);
    mdilHeader.magic                = 'MDIL';
    mdilHeader.version              = MDIL_VERSION_CURRENT;
    mdilHeader.methodMapCount       = m_methodRidCount;
    mdilHeader.extModuleCount       = m_extModRef.GetCount();
    mdilHeader.genericInstSize      = m_genericInstPool.GetCount();
    mdilHeader.extTypeCount         = m_extTypeRef.GetCount();
    mdilHeader.extMemberCount       = m_extMemberRef.GetCount();
    mdilHeader.namePoolSize         = m_namePool.GetCount();
    mdilHeader.codeSize             = totalCodeSize;
    mdilHeader.typeMapCount         = m_typeRidCount;
    mdilHeader.typeSpecCount        = m_typeSpecToOffs.GetCount();
    mdilHeader.methodSpecCount      = m_methodSpecToOffs.GetCount();
    mdilHeader.signatureCount       = m_signatureToOffs.GetCount();
    mdilHeader.typeSize             = m_compactLayoutOffs;
    mdilHeader.userStringPoolSize   = m_userStringPool.GetCount();
    mdilHeader.stubSize             = m_stubBuffer.GetCount();
    mdilHeader.stubAssocSize        = m_stubAssocBuffer.GetCount();
    mdilHeader.debugMapCount        = m_mapMethodRidToDebug.GetCount();
    mdilHeader.debugInfoSize        = totalDebugInfoSize;

    mdilHeader.genericCodeSize      = m_codeOffs[GENERIC_CODE];
    mdilHeader.genericDebugInfoSize = m_debugInfoBuffer[GENERIC_CODE].GetCount();

    mdilHeader.compilerVersionMajor = VER_MAJORVERSION;
    mdilHeader.compilerVersionMinor = VER_MINORVERSION;
    mdilHeader.compilerVersionBuildNumber = VER_PRODUCTBUILD;
    mdilHeader.compilerVersionPrivateBuildNumber = VER_PRODUCTBUILD_QFE;

    mdilHeader.subVersion           = MDIL_SUB_VERSION_CURRENT;

    if (m_wellKnownTypesTable.GetCount() != 0)
    {
        assert(m_wellKnownTypesTable.GetCount() == WKT_COUNT);
        mdilHeader.flags |= MDILHeader::WellKnownTypesPresent;
    }

    LoadHintEnum loadHint = LoadDefault;
    LoadHintEnum defaultLoadHint = LoadDefault;
    GetCompileInfo()->GetLoadHint(m_zapper->m_hAssembly,
                                  m_zapper->m_hAssembly,
                                  &loadHint,
                                  &defaultLoadHint);
    if (defaultLoadHint == LoadAlways)
    {
        mdilHeader.flags |= MDILHeader::IsEagerlyLoaded;
    }

    mdilHeader.flags |= GetCompileInfo()->GetMdilModuleSecurityFlags(m_zapper->m_hAssembly);

    if (GetCompileInfo()->CompilerRelaxationNoStringInterningPermitted(m_zapper->m_hAssembly))
    {
        mdilHeader.flags |= MDILHeader::CompilerRelaxationNoStringInterning;
    }

    if (GetCompileInfo()->CompilerRelaxationNoStringInterningPermitted(m_zapper->m_hAssembly))
    {
        mdilHeader.flags |= MDILHeader::RuntimeCompatibilityRuntimeWrappedExceptions;
    }

    if (m_zapper->m_pOpt->m_compilerFlags & CORJIT_FLG_MINIMAL_MDIL)
    {
        mdilHeader.flags |= MDILHeader::MinimalMDILImage;
    }

    if (m_zapper->m_pOpt->m_compilerFlags & CORJIT_FLG_NO_MDIL)
    {
        mdilHeader.flags |= MDILHeader::NoMDILImage;
    }

    mdilHeader.cerReliabilityContract = GetCompileInfo()->CERReliabilityContract(m_zapper->m_hAssembly);

    // reset architecture mask
    mdilHeader.flags &= ~MDILHeader::TargetArch_Mask;

#if defined(_TARGET_X86_)
    mdilHeader.flags |= MDILHeader::TargetArch_X86;
#elif defined(_TARGET_ARM_)
    mdilHeader.flags |= MDILHeader::TargetArch_ARM;
#elif defined(_TARGET_AMD64_)
    mdilHeader.flags |= MDILHeader::TargetArch_AMD64;
#else
#error unexpected target architecture (neither x86, ARM, or AMD64)
#endif //_TARGET_X86_

    mdilHeader.entryPointToken = m_ModuleDecoder.GetEntryPointToken();
    mdilHeader.subsystem = m_ModuleDecoder.GetSubsystem();
    {
        // Read the actual preferred base address from the disk

        // Note that we are reopening the file here. We are not guaranteed to get the same file.
        // The worst thing that can happen is that we will read a bogus preferred base address from the file.
        HandleHolder hFile(WszCreateFile(m_pModuleFileName,
                                            GENERIC_READ,
                                            FILE_SHARE_READ|FILE_SHARE_DELETE,
                                            NULL,
                                            OPEN_EXISTING,
                                            FILE_ATTRIBUTE_NORMAL,
                                            NULL));
        if (hFile == INVALID_HANDLE_VALUE)
            ThrowLastError();

        HandleHolder hFileMap(WszCreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL));
        if (hFileMap == NULL)
            ThrowLastError();

        MapViewHolder base(MapViewOfFile(hFileMap, FILE_MAP_READ, 0, 0, 0));
        if (base == NULL)
            ThrowLastError();
    
        DWORD dwFileLen = SafeGetFileSize(hFile, 0);
        if (dwFileLen == INVALID_FILE_SIZE)
            ThrowLastError();

        PEDecoder peFlat((void *)base, (COUNT_T)dwFileLen);

        mdilHeader.baseAddress = peFlat.GetPreferredBase();
    }

    mdilHeader.platformID = MDILHeader::PlatformID_Triton;
    
    ClrCtlData clrCtlData;
    SArray<BYTE> blobData;
    const void *pPublicKey = NULL;
    ULONG cbPublicKey = 0;
    ULONG cbPublicKeyToken = 0;
    BYTE* pKeyToken = NULL;

    AssemblyMetaDataInternal metaData;
    LPCSTR pModuleName;
    LPCSTR pAssemblyName;
    DWORD flags;
    memset(&clrCtlData, 0, sizeof(clrCtlData));
    clrCtlData.hdrSize = sizeof(clrCtlData);

    m_pMDImport->GetScopeProps(&pModuleName, &clrCtlData.MVID);
    m_pMDImport->GetAssemblyProps(
            TokenFromRid(1, mdtAssembly),       // [IN] The Assembly for which to get the properties.
            &pPublicKey,
            &cbPublicKey,
            NULL,                               // [OUT] Hash Algorithm
            &pAssemblyName,                     // [OUT] Buffer to fill with name
            &metaData,                          // [OUT] Assembly Metadata (version, locale, etc.)
            &flags);                            // [OUT] Flags

    clrCtlData.assemblyName = m_assemblyName;
    clrCtlData.locale = m_locale;
    clrCtlData.majorVersion = metaData.usMajorVersion;
    clrCtlData.minorVersion = metaData.usMinorVersion;
    clrCtlData.buildNumber = metaData.usBuildNumber;
    clrCtlData.revisionNumber = metaData.usRevisionNumber;
    if (cbPublicKey > 0) {
        if ((flags & afPublicKey)!= 0) {
            clrCtlData.hasPublicKey = 1;
        }
        clrCtlData.cbPublicKey = cbPublicKey;
        clrCtlData.publicKeyBlob = blobData.GetCount();
        blobData.SetCount(clrCtlData.publicKeyBlob + clrCtlData.cbPublicKey);
        memcpy_s(&blobData[(COUNT_T)clrCtlData.publicKeyBlob], clrCtlData.cbPublicKey, pPublicKey, cbPublicKey);

        if (StrongNameTokenFromPublicKey((BYTE*)pPublicKey, cbPublicKey,
                                     &pKeyToken, &cbPublicKeyToken))
        {
            if (cbPublicKeyToken > 0 && cbPublicKeyToken == sizeof(clrCtlData.publicKeyToken)) {
                memcpy(&clrCtlData.publicKeyToken, pKeyToken, cbPublicKeyToken);
                clrCtlData.cbPublicKeyToken = cbPublicKeyToken;
                clrCtlData.hasPublicKeyToken = true;
            }
        }

    }

    CORCOMPILE_VERSION_INFO versionInfo;
    IfFailThrow(m_zapper->m_pEECompileInfo->GetAssemblyVersionInfo(m_zapper->m_hAssembly, &versionInfo));

    mdilHeader.timeDateStamp = versionInfo.sourceAssembly.timeStamp;
    clrCtlData.ilImageSize = versionInfo.sourceAssembly.ilImageSize;
    clrCtlData.wcbSNHash = 0;
    clrCtlData.snHashBlob = blobData.GetCount();
    
    clrCtlData.cbTPBandName = 0;
    clrCtlData.tpBandNameBlob = blobData.GetCount();

    clrCtlData.extTypeRefExtendCount        = m_extTypeRefExtend.GetCount();
    clrCtlData.extMemberRefExtendCount      = m_extMemberRefExtend.GetCount();
    
    clrCtlData.neutralResourceCultureNameLen   = m_neutralResourceCultureNameLen;
    clrCtlData.neutralResourceCultureName      = m_cultureName;
    clrCtlData.neutralResourceFallbackLocation = m_neutralResourceFallbackLocation;

    mdilHeader.blobDataSize = blobData.GetCount() * sizeof(blobData[0]);

    if ((versionInfo.wConfigFlags & CORCOMPILE_CONFIG_DEBUG) != 0)
    {
        mdilHeader.flags |= MDILHeader::DebuggableMDILCode;
        if ((versionInfo.wConfigFlags & CORCOMPILE_CONFIG_DEBUG_DEFAULT) != 0)
            mdilHeader.flags |= MDILHeader::DebuggableILAssembly;
    }
    else
    {
        // Current CLR doesn't allow non-debuggable native image to be generated from debuggable assembly.
        _ASSERTE((versionInfo.wConfigFlags & CORCOMPILE_CONFIG_DEBUG_DEFAULT) != 0);
    }

    //-----------------------------------------------------------------------------------------
    // Write out the MDIL blob.
    //-----------------------------------------------------------------------------------------
    if (fwrite(&mdilHeader,               sizeof(mdilHeader),                                        1, outputFile) != 1) goto error;
    m_pMDImport->GetRvaOffsetData(&clrCtlData.firstMethodRvaOffset, &clrCtlData.methodDefRecordSize, &clrCtlData.methodDefCount,
        &clrCtlData.firstFieldRvaOffset, &clrCtlData.fieldRvaRecordSize, &clrCtlData.fieldRvaCount);
    if (fwrite(&clrCtlData,               sizeof(clrCtlData),                                        1, outputFile) != 1) goto error;

    if (m_zapper->m_pOpt->m_compilerFlags & CORJIT_FLG_NO_MDIL)
    {   
        // If this is a no MDIL image, we are already done.
        goto success;
    }

    if (blobData.GetCount() > 0)
    {
        if(fwrite(&blobData[0], blobData.GetCount() * sizeof(blobData[0]), 1, outputFile) != 1) goto error;
    }

    if (mdilHeader.flags & MDILHeader::WellKnownTypesPresent
     && fwrite(&m_wellKnownTypesTable[0], m_wellKnownTypesTable.GetCount()*sizeof(m_wellKnownTypesTable[0]), 1, outputFile) != 1) goto error;
    if (m_typeRidCount != 0
     && fwrite(&m_mapTypeRidToOffs[0],    m_typeRidCount           *sizeof(m_mapTypeRidToOffs[0]),   1, outputFile) != 1) goto error;
    if (fwrite(&m_mapMethodRidToOffs[0],  m_methodRidCount         *sizeof(m_mapMethodRidToOffs[0]), 1, outputFile) != 1) goto error;
    if (mdilHeader.genericInstSize != 0
     && fwrite(&m_genericInstPool[0],     mdilHeader.genericInstSize*sizeof(m_genericInstPool[0]), 1, outputFile) != 1) goto error;
    if (fwrite(&m_extModRef[0],           m_extModRef.GetCount()   *sizeof(m_extModRef[0]),          1, outputFile) != 1) goto error;
    if (fwrite(&m_extTypeRef[0],          m_extTypeRef.GetCount()  *sizeof(m_extTypeRef[0]),         1, outputFile) != 1) goto error;
    if (fwrite(&m_extMemberRef[0],        m_extMemberRef.GetCount()*sizeof(m_extMemberRef[0]),       1, outputFile) != 1) goto error;
    if (mdilHeader.typeSpecCount > 0
     && fwrite(&m_typeSpecToOffs[0],      m_typeSpecToOffs.GetCount()*sizeof(m_typeSpecToOffs[0]),   1, outputFile) != 1) goto error;
    if (mdilHeader.methodSpecCount > 0
     && fwrite(&m_methodSpecToOffs[0],    m_methodSpecToOffs.GetCount()*sizeof(m_methodSpecToOffs[0]),1,outputFile) != 1) goto error;
    if (mdilHeader.signatureCount > 0
     && fwrite(&m_signatureToOffs[0],     m_signatureToOffs.GetCount()*sizeof(m_signatureToOffs[0]), 1,outputFile) != 1) goto error;
    if (fwrite(&m_namePool[0],            sizeof(m_namePool[0]),   m_namePool.GetCount(),               outputFile) != m_namePool.GetCount()) goto error;
    if (m_compactLayoutOffs > 0
     && fwrite(&m_compactLayoutBuffer[0], m_compactLayoutOffs      *sizeof(m_compactLayoutBuffer[0]),1, outputFile) != 1) goto error;
    if (mdilHeader.userStringPoolSize > 0
     && fwrite(&m_userStringPool[0],      sizeof(m_userStringPool[0]),m_userStringPool.GetCount(),      outputFile) != m_userStringPool.GetCount()) goto error;
    if (fwrite(&m_codeBuffer[GENERIC_CODE][0], m_codeOffs[GENERIC_CODE],                             1, outputFile) != 1) goto error;
    //write out the non-generic code immediatly after the generic code.
    if (m_codeOffs[NON_GENERIC_CODE] != 0 && fwrite(&m_codeBuffer[NON_GENERIC_CODE][0], m_codeOffs[NON_GENERIC_CODE],                       1, outputFile) != 1) goto error;    
    if (mdilHeader.stubSize > 0
     && fwrite(&m_stubBuffer[0],          mdilHeader.stubSize*sizeof(m_stubBuffer[0]),               1, outputFile) != 1) goto error;
    if (mdilHeader.stubAssocSize > 0
     && fwrite(&m_stubAssocBuffer[0],     mdilHeader.stubAssocSize*sizeof(m_stubAssocBuffer[0]),     1, outputFile) != 1) goto error;
    if (mdilHeader.debugMapCount > 0
     && fwrite(&m_mapMethodRidToDebug[0], mdilHeader.debugMapCount*sizeof(m_mapMethodRidToDebug[0]), 1, outputFile) != 1) goto error;
    if (m_debugInfoBuffer[GENERIC_CODE].GetCount() > 0
     && fwrite(&m_debugInfoBuffer[GENERIC_CODE][0], m_debugInfoBuffer[GENERIC_CODE].GetCount(),      1, outputFile) != 1) goto error;
    //write out the non-generic debug info immediately after the generic debug info
    if (m_debugInfoBuffer[NON_GENERIC_CODE].GetCount() > 0
     && fwrite(&m_debugInfoBuffer[NON_GENERIC_CODE][0], m_debugInfoBuffer[NON_GENERIC_CODE].GetCount(),1, outputFile) != 1) goto error;


    if (m_extTypeRefExtend.GetCount() > 0) 
    {
        if (fwrite(&m_extTypeRefExtend[0], m_extTypeRefExtend.GetCount()*sizeof(m_extTypeRefExtend[0]), 1, outputFile) != 1) goto error;
    }
    if (m_extMemberRefExtend.GetCount() > 0) 
    {
        if (fwrite(&m_extMemberRefExtend[0], m_extMemberRefExtend.GetCount()*sizeof(m_extMemberRefExtend[0]), 1, outputFile) != 1) goto error;
    }

    
success:
    return ERROR_SUCCESS;

error:
    DWORD dwLastError = GetLastError();
    if (dwLastError == ERROR_SUCCESS)
        dwLastError = E_FAIL;
    return dwLastError;
#endif // BINDER
}

void ZapImage::FlushCompactLayoutData(mdToken typeToken, BYTE *pData, ULONG cData)
{
#ifndef BINDER
    // Save the data in m_compactLayoutBuffer
    COUNT_T dataSize = m_compactLayoutBuffer.GetCount();
    if (dataSize < sizeof(DWORD))
    {
        assert(dataSize == 0);
        m_compactLayoutBuffer.SetCount(10000);
        memcpy(&m_compactLayoutBuffer[0], "CMPL", sizeof(DWORD));
        m_compactLayoutOffs = sizeof(DWORD);
    }
    COUNT_T desiredSize = m_compactLayoutOffs + cData;
    while (m_compactLayoutBuffer.GetCount() < desiredSize)
        m_compactLayoutBuffer.SetCount(m_compactLayoutBuffer.GetCount()*2);
    memcpy(&m_compactLayoutBuffer[(COUNT_T)m_compactLayoutOffs], pData, cData);

    COUNT_T rid = RidFromToken(typeToken);
    if (TypeFromToken(typeToken) == mdtTypeSpec)
    {
        assert(rid < m_typeSpecToOffs.GetCount());
        m_typeSpecToOffs[rid] = m_compactLayoutOffs;
    }
    else if (TypeFromToken(typeToken) == mdtMethodSpec)
    {
        assert(rid < m_methodSpecToOffs.GetCount());
        m_methodSpecToOffs[rid] = m_compactLayoutOffs;
    }
    else if (TypeFromToken(typeToken) == mdtSignature)
    {
        assert(rid < m_signatureToOffs.GetCount());
        m_signatureToOffs[rid] = m_compactLayoutOffs;
    }
    else if (TypeFromToken(typeToken) == mdtMemberRef)
    {
        assert(rid < m_extMemberRefExtend.GetCount());
        m_extMemberRefExtend[rid].signature = m_compactLayoutOffs;
    }
    else
    {
        assert(TypeFromToken(typeToken) == mdtTypeDef);
        // Remember the offset in m_mapTypeRidToOffs
        COUNT_T mappingCount = m_mapTypeRidToOffs.GetCount();
        if (mappingCount <= rid)
        {
            if (mappingCount == 0)
            {
                m_typeRidCount = 0;
                m_mapTypeRidToOffs.SetCount(1000);
            }
            while (m_mapTypeRidToOffs.GetCount() <= rid)
                m_mapTypeRidToOffs.SetCount(m_mapTypeRidToOffs.GetCount()*2);
            COUNT_T newMappingCount = m_mapTypeRidToOffs.GetCount();
            for (COUNT_T i = mappingCount; i < newMappingCount; i++)
                m_mapTypeRidToOffs[i] = 0;
            m_typeRidCount = rid+1;
        }
        if (m_typeRidCount < rid+1)
            m_typeRidCount = rid+1;
        m_mapTypeRidToOffs[rid] = m_compactLayoutOffs;
    }
    m_compactLayoutOffs += cData;
#endif // !BINDER
}

void ZapImage::FlushStubData(BYTE *pStubSize, ULONG cStubSize,
                             BYTE *pStubData, ULONG cStubData,
                             BYTE *pStubAssocData, ULONG cStubAssocData)
{
    // Save the data in m_stubBuffer and m_stubAssocBuffer
    m_stubBuffer.SetCount(cStubSize + cStubData);
    memcpy(&m_stubBuffer[0], pStubSize, cStubSize);
    memcpy(&m_stubBuffer[(COUNT_T)cStubSize], pStubData, cStubData);

    m_stubAssocBuffer.SetCount(cStubAssocData);
    memcpy(&m_stubAssocBuffer[0], pStubAssocData, cStubAssocData);
}

// Flush the user string pool
void ZapImage::FlushUserStringPool(BYTE *pData, ULONG cData)
{
    m_userStringPool.SetCount(AlignUp(cData, sizeof(DWORD)));
    memcpy(&m_userStringPool[0], pData, cData);
}

void ZapImage::FlushWellKnownTypes(DWORD *wellKnownTypesTable, SIZE_T count)
{
    m_wellKnownTypesTable.SetCount((DWORD)count);
    memcpy(&m_wellKnownTypesTable[0], wellKnownTypesTable, count*sizeof(wellKnownTypesTable[0]));
}
#endif

void ZapImage::Compile()
{
    //
    // First, compile methods in the load order array.
    //
    bool doNothingNgen = false;
#ifdef _DEBUG
    static ConfigDWORD fDoNothingNGen;
    doNothingNgen = !!fDoNothingNGen.val(CLRConfig::INTERNAL_ZapDoNothing);
#endif

#ifdef MDIL
    // Reset stream (buffer) only when we are really generating MDIL (instead of just an empty MDIL section)
    if ((m_zapper->m_pOpt->m_compilerFlags & (CORJIT_FLG_MDIL|CORJIT_FLG_NO_MDIL)) == CORJIT_FLG_MDIL)
    {
        GetCompactLayoutWriter()->Reset();
    }
#endif

    if (!doNothingNgen)
    {
        //
        // Compile the methods specified by the IBC profile data
        // 
        CompileProfileData();

        BeginRegion(CORINFO_REGION_COLD);


        IMDInternalImport * pMDImport = m_pMDImport;

        HENUMInternalHolder hEnum(pMDImport);
        hEnum.EnumAllInit(mdtMethodDef);

        mdMethodDef md;
        while (pMDImport->EnumNext(&hEnum, &md))
        {
            if (m_pILMetaData != NULL)
            {
                // Copy IL for all methods. We treat errors during copying IL 
                // over as fatal error. These errors are typically caused by 
                // corrupted IL images.
                // 
                m_pILMetaData->EmitMethodIL(md);
            }

            //
            // Compile the remaining methods that weren't compiled during the CompileProfileData phase
            //
            TryCompileMethodDef(md, 0);
        }

        // Compile any generic code which lands in this LoaderModule
        // that resulted from the above compilations
        CORINFO_METHOD_HANDLE handle = m_pPreloader->NextUncompiledMethod();
        while (handle != NULL)
        {
            TryCompileInstantiatedMethod(handle, 0);
            handle = m_pPreloader->NextUncompiledMethod();
        }

        EndRegion(CORINFO_REGION_COLD);

        // If we want ngen to fail when we create partial ngen images we can
        // throw an NGEN failure HRESULT here.
#if 0
        if (m_zapper->m_failed)
        {
            ThrowHR(NGEN_E_TP_PARTIAL_IMAGE); 
        }
#endif

    }

    // Compute a preferred class layout order based on analyzing the graph
    // of which classes contain calls to other classes.
    ComputeClassLayoutOrder();

    // Sort the unprofiled methods by this preferred class layout, if available
    if (m_fHasClassLayoutOrder)
    {
        SortUnprofiledMethodsByClassLayoutOrder();
    }

#ifdef MDIL
    if (m_zapper->m_pOpt->m_compilerFlags & CORJIT_FLG_MDIL)
    {
        if (!(m_zapper->m_pOpt->m_compilerFlags & CORJIT_FLG_NO_MDIL))
        {
            GetCompactLayoutWriter()->FlushStubData();
        }
        Output_MDIL();
    }
    else
#endif
    {
        if (IsReadyToRunCompilation())
        {
            // Pretend that no methods are trained, so that everything is in single code section
            // READYTORUN: FUTURE: More than one code section
            m_iUntrainedMethod = 0;
        }

        OutputCode(ProfiledHot);
        OutputCode(Unprofiled);
        OutputCode(ProfiledCold);

        OutputCodeInfo(ProfiledHot);
        OutputCodeInfo(ProfiledCold);  // actually both Unprofiled and ProfiledCold

        OutputGCInfo();
        OutputProfileData();

#ifdef FEATURE_READYTORUN_COMPILER
        if (IsReadyToRunCompilation())
        {
            OutputEntrypointsTableForReadyToRun();
            OutputDebugInfoForReadyToRun();
        }
        else
#endif
        {
            OutputDebugInfo();
        }
    }
}

struct CompileMethodStubContext
{
    ZapImage *                  pImage;
    unsigned                    methodProfilingDataFlags;
    ZapImage::CompileStatus     enumCompileStubResult;

    CompileMethodStubContext(ZapImage * _image, unsigned _methodProfilingDataFlags)
    {
        pImage                   = _image;
        methodProfilingDataFlags = _methodProfilingDataFlags;
        enumCompileStubResult    = ZapImage::NOT_COMPILED;
    }
};

//-----------------------------------------------------------------------------
// This method is a callback function use to compile any IL_STUBS that are
// associated with a normal IL method.  It is called from CompileMethodStubIfNeeded
// via the function pointer stored in the CompileMethodStubContext.
// It handles the temporary change to the m_compilerFlags and removes any flags
// that we don't want set when compiling IL_STUBS.
//-----------------------------------------------------------------------------

// static void __stdcall 
void ZapImage::TryCompileMethodStub(LPVOID pContext, CORINFO_METHOD_HANDLE hStub, DWORD dwJitFlags)
{
    STANDARD_VM_CONTRACT;

    // The caller must always set the IL_STUB flag
    _ASSERTE((dwJitFlags & CORJIT_FLG_IL_STUB) != 0);

    CompileMethodStubContext *pCompileContext = reinterpret_cast<CompileMethodStubContext *>(pContext);
    ZapImage *pImage = pCompileContext->pImage;

    unsigned oldFlags = pImage->m_zapper->m_pOpt->m_compilerFlags;

    pImage->m_zapper->m_pOpt->m_compilerFlags |= dwJitFlags;
    pImage->m_zapper->m_pOpt->m_compilerFlags &= ~(CORJIT_FLG_PROF_ENTERLEAVE | 
                                                   CORJIT_FLG_DEBUG_CODE | 
                                                   CORJIT_FLG_DEBUG_EnC | 
                                                   CORJIT_FLG_DEBUG_INFO);

    mdMethodDef md = mdMethodDefNil;
#ifdef MDIL
    if (pImage->m_zapper->m_pOpt->m_compilerFlags & CORJIT_FLG_MDIL)
    {
        md = pImage->GetCompactLayoutWriter()->GetNextStubToken();
        if (md == mdMethodDefNil)
            return;

        pImage->m_stubMethodCount++;
    }
#endif // MDIL

    pCompileContext->enumCompileStubResult = pImage->TryCompileMethodWorker(hStub, md,
                                                         pCompileContext->methodProfilingDataFlags);

    pImage->m_zapper->m_pOpt->m_compilerFlags = oldFlags;
}

//-----------------------------------------------------------------------------
// Helper for ZapImage::TryCompileMethodDef that indicates whether a given method def token refers to a
// "vtable gap" method. These are pseudo-methods used to lay out the vtable for COM interop and as such don't
// have any associated code (or even a method handle).
//-----------------------------------------------------------------------------
BOOL ZapImage::IsVTableGapMethod(mdMethodDef md)
{
#ifdef FEATURE_COMINTEROP 
    HRESULT hr;
    DWORD dwAttributes;

    // Get method attributes and check that RTSpecialName was set for the method (this means the name has
    // semantic import to the runtime and must be formatted rigorously with one of a few well known rules).
    // Note that we just return false on any failure path since this will just lead to our caller continuing
    // to throw the exception they were about to anyway.
    hr = m_pMDImport->GetMethodDefProps(md, &dwAttributes);
    if (FAILED(hr) || !IsMdRTSpecialName(dwAttributes))
        return FALSE;

    // Now check the name of the method. All vtable gap methods will have a prefix of "_VtblGap".
    LPCSTR szMethod;
    PCCOR_SIGNATURE pvSigBlob;
    ULONG cbSigBlob;    
    hr = m_pMDImport->GetNameAndSigOfMethodDef(md, &pvSigBlob, &cbSigBlob, &szMethod);
    if (FAILED(hr) || (strncmp(szMethod, "_VtblGap", 8) != 0))
        return FALSE;

    // If we make it to here we have a vtable gap method.
    return TRUE;
#else
    return FALSE;
#endif // FEATURE_COMINTEROP
}

//-----------------------------------------------------------------------------
// This function is called for non-generic methods in the current assembly,
// and for the typical "System.__Canon" instantiations of generic methods
// in the current assembly.
//-----------------------------------------------------------------------------

ZapImage::CompileStatus ZapImage::TryCompileMethodDef(mdMethodDef md, unsigned methodProfilingDataFlags)
{
    _ASSERTE(!IsNilToken(md));

    CORINFO_METHOD_HANDLE handle = NULL;
    CompileStatus         result = NOT_COMPILED;

    EX_TRY
    {
        if (ShouldCompileMethodDef(md))
            handle = m_pPreloader->LookupMethodDef(md);
        else
            result = COMPILE_EXCLUDED;
    }
    EX_CATCH
    {
        // Continue unwinding if fatal error was hit.
        if (FAILED(g_hrFatalError))
            ThrowHR(g_hrFatalError);

        // COM introduces the notion of a vtable gap method, which is not a real method at all but instead
        // aids in the explicit layout of COM interop vtables. These methods have no implementation and no
        // direct runtime state tracking them. Trying to lookup a method handle for a vtable gap method will
        // throw an exception but we choose to let that happen and filter out the warning here in the
        // handler because (a) vtable gap methods are rare and (b) it's not all that cheap to identify them
        // beforehand.
        if (IsVTableGapMethod(md))
        {
            handle = NULL;
        }
        else
        {
#ifndef BINDER
            Exception *ex = GET_EXCEPTION();
            HRESULT hrException = ex->GetHR();

            StackSString message;
            if (hrException != COR_E_UNSUPPORTEDMDIL)
                ex->GetMessage(message);

            CorZapLogLevel level;

#ifdef CROSSGEN_COMPILE
            // Warnings should not go to stderr during crossgen
            level = CORZAP_LOGLEVEL_WARNING;
#else
            level = CORZAP_LOGLEVEL_ERROR;
#endif

            // FileNotFound errors here can be converted into a single error string per ngen compile, and the detailed error is available with verbose logging
            if (hrException == COR_E_FILENOTFOUND)
            {
                StackSString logMessage(W("System.IO.FileNotFoundException: "));
                logMessage.Append(message);
                FileNotFoundError(logMessage.GetUnicode());
                level = CORZAP_LOGLEVEL_INFO;
            }

            if (hrException != COR_E_UNSUPPORTEDMDIL)
                m_zapper->Print(level, W("%s while compiling method token 0x%x\n"), message.GetUnicode(), md);
#else
            m_zapper->PrintErrorMessage(CORZAP_LOGLEVEL_ERROR, GET_EXCEPTION());
            m_zapper->Error(W(" while compiling method token 0x%x\n"), md);
#endif

            result = LOOKUP_FAILED;

            m_zapper->m_failed = TRUE;
            if (m_stats)
                m_stats->m_failedMethods++;
        }
    }
    EX_END_CATCH(SwallowAllExceptions);

    if (handle == NULL)
        return result;

    // compile the method
    //
    CompileStatus methodCompileStatus = TryCompileMethodWorker(handle, md, methodProfilingDataFlags);

    // Don't bother compiling the IL_STUBS if we failed to compile the parent IL method
    //
    if (methodCompileStatus == COMPILE_SUCCEED)
    {
        CompileMethodStubContext context(this, methodProfilingDataFlags);

        // compile stubs associated with the method
        m_pPreloader->GenerateMethodStubs(handle, m_zapper->m_pOpt->m_ngenProfileImage,
                                          &TryCompileMethodStub,
                                          &context);

#ifdef  MDIL
        if (m_zapper->m_pOpt->m_compilerFlags & CORJIT_FLG_MDIL)
            m_pPreloader->AddMDILCodeFlavorsToUncompiledMethods(handle);
#endif

    }

    return methodCompileStatus;
}


//-----------------------------------------------------------------------------
// This function is called for non-"System.__Canon" instantiations of generic methods.
// These could be methods defined in other assemblies too.
//-----------------------------------------------------------------------------

ZapImage::CompileStatus ZapImage::TryCompileInstantiatedMethod(CORINFO_METHOD_HANDLE handle, 
                                                               unsigned methodProfilingDataFlags)
{
    // READYTORUN: FUTURE: Generics
    if (IsReadyToRunCompilation())
        return COMPILE_EXCLUDED;

    if (!ShouldCompileInstantiatedMethod(handle))
        return COMPILE_EXCLUDED;

    // If we compiling this method because it was specified by the IBC profile data
    // then issue an warning if this method is not on our uncompiled method list
    // 
    if (methodProfilingDataFlags != 0)
    {
        if (methodProfilingDataFlags & (1 << ReadMethodCode))
        {
            // When we have stale IBC data the method could have been rejected from this image.
            if (!m_pPreloader->IsUncompiledMethod(handle))
            {
                const char* szClsName;
                const char* szMethodName = m_zapper->m_pEEJitInfo->getMethodName(handle, &szClsName);

                SString fullname(SString::Utf8, szClsName);
                fullname.AppendUTF8(NAMESPACE_SEPARATOR_STR);
                fullname.AppendUTF8(szMethodName);

                m_zapper->Info(W("Warning: Invalid method instantiation in profile data: %s\n"), fullname.GetUnicode());

                return NOT_COMPILED;
            }
        }
    }
   
    CompileStatus methodCompileStatus = TryCompileMethodWorker(handle, mdMethodDefNil, methodProfilingDataFlags);

    // Don't bother compiling the IL_STUBS if we failed to compile the parent IL method
    //
    if (methodCompileStatus == COMPILE_SUCCEED)
    {
        CompileMethodStubContext context(this, methodProfilingDataFlags);

        // compile stubs associated with the method
        m_pPreloader->GenerateMethodStubs(handle, m_zapper->m_pOpt->m_ngenProfileImage,
                                          &TryCompileMethodStub, 
                                          &context);
    }

    return methodCompileStatus;
}

//-----------------------------------------------------------------------------

ZapImage::CompileStatus ZapImage::TryCompileMethodWorker(CORINFO_METHOD_HANDLE handle, mdMethodDef md, 
                                                         unsigned methodProfilingDataFlags)
{
    _ASSERTE(handle != NULL);

    if (m_zapper->m_pOpt->m_onlyOneMethod && (m_zapper->m_pOpt->m_onlyOneMethod != md))
        return NOT_COMPILED;

#ifdef MDIL
    // This is a quick workaround to opt specific methods out of MDIL generation to work around bugs.
    if (m_zapper->m_pOpt->m_compilerFlags & CORJIT_FLG_MDIL)
    {
        HRESULT hr = m_pMDImport->GetCustomAttributeByName(md, "System.Runtime.BypassMdilAttribute", NULL, NULL);
        if (hr == S_OK)
            return NOT_COMPILED;
    }
#endif

#ifdef FEATURE_READYTORUN_COMPILER
    // This is a quick workaround to opt specific methods out of ReadyToRun compilation to work around bugs.
    if (IsReadyToRunCompilation())
    {
        HRESULT hr = m_pMDImport->GetCustomAttributeByName(md, "System.Runtime.BypassReadyToRun", NULL, NULL);
        if (hr == S_OK)
            return NOT_COMPILED;
    }
#endif

    if (methodProfilingDataFlags != 0)
    {
        // Report the profiling data flags for layout of the EE datastructures
        m_pPreloader->SetMethodProfilingFlags(handle, methodProfilingDataFlags);

        // Only proceed with compilation if the code is hot
        //
        if ((methodProfilingDataFlags & (1 << ReadMethodCode)) == 0)
            return NOT_COMPILED;
    }
    else
    {
        if (m_zapper->m_pOpt->m_fPartialNGen)
            return COMPILE_EXCLUDED;
    }

    // Have we already compiled it?
    if (GetCompiledMethod(handle) != NULL)
        return ALREADY_COMPILED;

    _ASSERTE((m_zapper->m_pOpt->m_compilerFlags & CORJIT_FLG_IL_STUB) || IsNilToken(md) || handle == m_pPreloader->LookupMethodDef(md));

    CompileStatus result = NOT_COMPILED;
    
    // This is an entry point into the JIT which can call back into the VM. There are methods in the
    // JIT that will swallow exceptions and only the VM guarentees that exceptions caught or swallowed
    // with restore the debug state of the stack guards. So it is necessary to ensure that the status
    // is restored on return from the call into the JIT, which this light-weight transition macro
    // will do.
    REMOVE_STACK_GUARD;

    CORINFO_MODULE_HANDLE module;

    // We only compile IL_STUBs from the current assembly
    if (m_zapper->m_pOpt->m_compilerFlags & CORJIT_FLG_IL_STUB)
        module = m_hModule;
    else
        module = m_zapper->m_pEEJitInfo->getMethodModule(handle);

    ZapInfo zapInfo(this, md, handle, module, methodProfilingDataFlags);

    EX_TRY
    {
        zapInfo.CompileMethod();
        result = COMPILE_SUCCEED;
    }
    EX_CATCH
    {
#ifndef BINDER
        // Continue unwinding if fatal error was hit.
        if (FAILED(g_hrFatalError))
            ThrowHR(g_hrFatalError);

        Exception *ex = GET_EXCEPTION();
        HRESULT hrException = ex->GetHR();

        StackSString message;
        if (hrException != COR_E_UNSUPPORTEDMDIL)
            ex->GetMessage(message);

        CorZapLogLevel level;

#ifdef CROSSGEN_COMPILE
        // Warnings should not go to stderr during crossgen
        level = CORZAP_LOGLEVEL_WARNING;
#else
        level = CORZAP_LOGLEVEL_ERROR;
#endif

        // FileNotFound errors here can be converted into a single error string per ngen compile, and the detailed error is available with verbose logging
        if (hrException == COR_E_FILENOTFOUND)
        {
            StackSString logMessage(W("System.IO.FileNotFoundException: "));
            logMessage.Append(message);
            FileNotFoundError(logMessage.GetUnicode());
            level = CORZAP_LOGLEVEL_INFO;
        }

        if (hrException != COR_E_UNSUPPORTEDMDIL)
            m_zapper->Print(level, W("%s while compiling method %s\n"), message.GetUnicode(), zapInfo.m_currentMethodName.GetUnicode());
#else
        m_zapper->PrintErrorMessage(CORZAP_LOGLEVEL_ERROR, GET_EXCEPTION());
        m_zapper->Error(W(" while compiling method %s\n"), zapInfo.m_currentMethodName.GetUnicode());
#endif
        result = COMPILE_FAILED;
        m_zapper->m_failed = TRUE;

        if (m_stats != NULL)
        {
            if ((m_zapper->m_pOpt->m_compilerFlags & CORJIT_FLG_IL_STUB) == 0)
                m_stats->m_failedMethods++;
            else
                m_stats->m_failedILStubs++;
        }
    }
    EX_END_CATCH(SwallowAllExceptions);
    
    return result;
}


// Should we compile this method, defined in the ngen'ing module?
// Result is FALSE if any of the controls (only used by prejit.exe) exclude the method
BOOL ZapImage::ShouldCompileMethodDef(mdMethodDef md)
{
    DWORD partialNGenStressVal = PartialNGenStressPercentage();
    if (partialNGenStressVal &&
        // Module::AddCerListToRootTable has problems if mscorlib.dll is
        // a partial ngen image
        m_hModule != m_zapper->m_pEECompileInfo->GetLoaderModuleForMscorlib())
    {
        _ASSERTE(partialNGenStressVal <= 100);
        DWORD methodPercentageVal = (md % 100) + 1;
        if (methodPercentageVal <= partialNGenStressVal)
            return FALSE;
    }
    
    mdTypeDef td;
    IfFailThrow(m_pMDImport->GetParentToken(md, &td));
    
#ifdef FEATURE_COMINTEROP
    mdToken tkExtends;
    if (td != mdTypeDefNil)
    {
        m_pMDImport->GetTypeDefProps(td, NULL, &tkExtends);
        
        mdAssembly tkAssembly;
        DWORD dwAssemblyFlags;
        
        m_pMDImport->GetAssemblyFromScope(&tkAssembly);
        if (TypeFromToken(tkAssembly) == mdtAssembly)
        {
            m_pMDImport->GetAssemblyProps(tkAssembly,
                                            NULL, NULL,     // Public Key
                                            NULL,           // Hash Algorithm
                                            NULL,           // Name
                                            NULL,           // MetaData
                                            &dwAssemblyFlags);
            
            if (IsAfContentType_WindowsRuntime(dwAssemblyFlags))
            {
                if (TypeFromToken(tkExtends) == mdtTypeRef)
                {
                    LPCSTR szNameSpace = NULL;
                    LPCSTR szName = NULL;
                    m_pMDImport->GetNameOfTypeRef(tkExtends, &szNameSpace, &szName);
                    
                    if (!strcmp(szNameSpace, "System") && !_stricmp((szName), "Attribute"))
                    {
                        return FALSE;
                    }
                }
            }
        }
    }
#endif

#ifdef _DEBUG
    static ConfigMethodSet fZapOnly;
    fZapOnly.ensureInit(CLRConfig::INTERNAL_ZapOnly);

    static ConfigMethodSet fZapExclude;
    fZapExclude.ensureInit(CLRConfig::INTERNAL_ZapExclude);

    PCCOR_SIGNATURE pvSigBlob;
    ULONG cbSigBlob;

    // Get the name of the current method and its class
    LPCSTR szMethod;
    IfFailThrow(m_pMDImport->GetNameAndSigOfMethodDef(md, &pvSigBlob, &cbSigBlob, &szMethod));
    
    LPCWSTR wszClass = W("");
    SString sClass;

    if (td != mdTypeDefNil)
    {
        LPCSTR szNameSpace = NULL;
        LPCSTR szName = NULL;
        
        IfFailThrow(m_pMDImport->GetNameOfTypeDef(td, &szName, &szNameSpace));
        
        const SString nameSpace(SString::Utf8, szNameSpace);
        const SString name(SString::Utf8, szName);
        sClass.MakeFullNamespacePath(nameSpace, name);
        wszClass = sClass.GetUnicode();
    }

    MAKE_UTF8PTR_FROMWIDE(szClass,  wszClass);

    if (!fZapOnly.isEmpty() && !fZapOnly.contains(szMethod, szClass, pvSigBlob))
    {
        LOG((LF_ZAP, LL_INFO1000, "Rejecting compilation of method %08x, %s::%s\n", md, szClass, szMethod));
        return FALSE;
    }

    if (fZapExclude.contains(szMethod, szClass, pvSigBlob))
    {
        LOG((LF_ZAP, LL_INFO1000, "Rejecting compilation of method %08x, %s::%s\n", md, szClass, szMethod));
        return FALSE;
    }

    LOG((LF_ZAP, LL_INFO1000, "Compiling method %08x, %s::%s\n", md, szClass, szMethod));
#endif    
    
    return TRUE;
}


BOOL ZapImage::ShouldCompileInstantiatedMethod(CORINFO_METHOD_HANDLE handle)
{
    DWORD partialNGenStressVal = PartialNGenStressPercentage();
    if (partialNGenStressVal &&
        // Module::AddCerListToRootTable has problems if mscorlib.dll is
        // a partial ngen image
        m_hModule != m_zapper->m_pEECompileInfo->GetLoaderModuleForMscorlib())
    {
        _ASSERTE(partialNGenStressVal <= 100);
        DWORD methodPercentageVal = (m_zapper->m_pEEJitInfo->getMethodHash(handle) % 100) + 1;
        if (methodPercentageVal <= partialNGenStressVal)
            return FALSE;
    }

    return TRUE;
}

HRESULT ZapImage::PrintTokenDescription(CorZapLogLevel level, mdToken token)
{
    HRESULT hr;

    if (RidFromToken(token) == 0)
        return S_OK;

    LPCSTR szNameSpace = NULL;
    LPCSTR szName = NULL;

    if (m_pMDImport->IsValidToken(token))
    {
        switch (TypeFromToken(token))
        {
            case mdtMemberRef:
            {
                mdToken parent;
                IfFailRet(m_pMDImport->GetParentOfMemberRef(token, &parent));
                if (RidFromToken(parent) != 0)
                {
                    PrintTokenDescription(level, parent);
                    m_zapper->Print(level, W("."));
                }
                IfFailRet(m_pMDImport->GetNameAndSigOfMemberRef(token, NULL, NULL, &szName));
                break;
            }

            case mdtMethodDef:
            {
                mdToken parent;
                IfFailRet(m_pMDImport->GetParentToken(token, &parent));
                if (RidFromToken(parent) != 0)
                {
                    PrintTokenDescription(level, parent);
                    m_zapper->Print(level, W("."));
                }
                IfFailRet(m_pMDImport->GetNameOfMethodDef(token, &szName));
                break;
            }

            case mdtTypeRef:
            {   
                IfFailRet(m_pMDImport->GetNameOfTypeRef(token, &szNameSpace, &szName));
                break;
            }

            case mdtTypeDef:
            {
                IfFailRet(m_pMDImport->GetNameOfTypeDef(token, &szName, &szNameSpace));
                break;
            }

            default:
                break;
        }      
    }
    else
    {
        szName = "InvalidToken";
    }

    SString fullName;

    if (szNameSpace != NULL)
    {
        const SString nameSpace(SString::Utf8, szNameSpace);
        const SString name(SString::Utf8, szName);
        fullName.MakeFullNamespacePath(nameSpace, name);
    }
    else
    {
        fullName.SetUTF8(szName);
    }

#ifdef BINDER
    m_zapper->Error(W("%s"), fullName.GetUnicode());
#else
    m_zapper->Print(level, W("%s"), fullName.GetUnicode());
#endif

    return S_OK;
}


HRESULT ZapImage::LocateProfileData()
{
    if (m_zapper->m_pOpt->m_ignoreProfileData)
    {
        return S_FALSE;
    }

    //
    // In the past, we have ignored profile data when instrumenting the assembly.
    // However, this creates significant differences between the tuning image and the eventual
    // optimized image (e.g. generic instantiations) which in turn leads to missed data during
    // training and cold touches during execution.  Instead, we take advantage of any IBC data
    // the assembly already has and attempt to make the tuning image as close as possible to
    // the final image.
    //
#if 0
    if ((m_zapper->m_pOpt->m_compilerFlags & CORJIT_FLG_BBINSTR) != 0)
        return S_FALSE;
#endif

    //
    // Don't use IBC data from untrusted assemblies--this allows us to assume that
    // the IBC data is not malicious
    //
    if (m_zapper->m_pEEJitInfo->canSkipVerification(m_hModule) != CORINFO_VERIFICATION_CAN_SKIP)
    {
        return S_FALSE;
    }

#if !defined(FEATURE_CORECLR) || defined(FEATURE_WINDOWSPHONE)
    //
    // See if there's profile data in the resource section of the PE
    //
    m_pRawProfileData = (BYTE*)m_ModuleDecoder.GetWin32Resource(W("PROFILE_DATA"), W("IBC"), &m_cRawProfileData);

    if ((m_pRawProfileData != NULL) && (m_cRawProfileData != 0))
    {
        m_zapper->Info(W("Found embedded profile resource in %s.\n"), m_pModuleFileName);
        return S_OK;
    }

    static ConfigDWORD g_UseIBCFile;
    if (g_UseIBCFile.val(CLRConfig::EXTERNAL_UseIBCFile) != 1)
        return S_OK;
#endif

    //
    // Couldn't find profile resource--let's see if there's an ibc file to use instead
    //

    SString path(m_pModuleFileName);

    SString::Iterator dot = path.End();
    if (path.FindBack(dot, '.'))
    {
        SString slName(SString::Literal, "ibc");
        path.Replace(dot+1, path.End() - (dot+1), slName);

        HandleHolder hFile = WszCreateFile(path.GetUnicode(),
                                     GENERIC_READ,
                                     FILE_SHARE_READ,
                                     NULL,
                                     OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                     NULL);
        if (hFile != INVALID_HANDLE_VALUE)
        {
            HandleHolder hMapFile = WszCreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
            DWORD dwFileLen = SafeGetFileSize(hFile, 0);
            if (dwFileLen != INVALID_FILE_SIZE)
            {
                if (hMapFile == NULL)
                {
                    m_zapper->Warning(W("Found profile data file %s, but could not open it"), path.GetUnicode());
                }
                else
                {
                    m_zapper->Info(W("Found ibc file %s.\n"), path.GetUnicode());

                    m_profileDataFile  = (BYTE*) MapViewOfFile(hMapFile, FILE_MAP_READ, 0, 0, 0);

                    m_pRawProfileData  = m_profileDataFile;
                    m_cRawProfileData  = dwFileLen;
                }
            }
        }
    }

    return S_OK;
}


bool ZapImage::CanConvertIbcData()
{
    static ConfigDWORD g_iConvertIbcData;
    DWORD val = g_iConvertIbcData.val(CLRConfig::UNSUPPORTED_ConvertIbcData);
    return (val != 0);
}

HRESULT ZapImage::parseProfileData()
{
    if (m_pRawProfileData == NULL)
    {
        return S_FALSE;
    }

    ProfileReader profileReader(m_pRawProfileData, m_cRawProfileData);

    CORBBTPROF_FILE_HEADER *fileHeader;

    READ(fileHeader, CORBBTPROF_FILE_HEADER);
    if (fileHeader->HeaderSize < sizeof(CORBBTPROF_FILE_HEADER))
    {
        _ASSERTE(!"HeaderSize is too small");
        return E_FAIL;
    }

    // Read any extra header data. It will be needed for V3 files.

    DWORD extraHeaderDataSize = fileHeader->HeaderSize - sizeof(CORBBTPROF_FILE_HEADER);
    void *extraHeaderData = profileReader.Read(extraHeaderDataSize);

    bool convertFromV1 = false;
    bool minified = false;

    if (fileHeader->Magic != CORBBTPROF_MAGIC) 
    {
        _ASSERTE(!"ibcHeader contains bad values");
        return E_FAIL;
    }

    // CoreCLR should never be presented with V1 IBC data.
#ifndef FEATURE_CORECLR
    if ((fileHeader->Version == CORBBTPROF_V1_VERSION) && CanConvertIbcData())
    {
        // Read and convert V1 data
        m_zapper->Info(W("Converting V1 IBC data to latest format.\n"));
        convertFromV1 = true;
    }
    else
#endif
    if (fileHeader->Version == CORBBTPROF_V3_VERSION)
    {
        CORBBTPROF_FILE_OPTIONAL_HEADER *optionalHeader =
            (CORBBTPROF_FILE_OPTIONAL_HEADER *)extraHeaderData;

        if (!optionalHeader ||
            !CONTAINS_FIELD(optionalHeader, extraHeaderDataSize, Size) ||
            (optionalHeader->Size > extraHeaderDataSize))
        {
            m_zapper->Info(W("Optional header missing or corrupt."));
            return E_FAIL;
        }

        if (CONTAINS_FIELD(optionalHeader, optionalHeader->Size, FileFlags))
        {
            minified = !!(optionalHeader->FileFlags & CORBBTPROF_FILE_FLAG_MINIFIED);

            if (!m_zapper->m_pOpt->m_fPartialNGenSet)
            {
                m_zapper->m_pOpt->m_fPartialNGen = !!(optionalHeader->FileFlags & CORBBTPROF_FILE_FLAG_PARTIAL_NGEN);
            }
        }
    }
    else if (fileHeader->Version != CORBBTPROF_V2_VERSION)
    {
        m_zapper->Info(W("Discarding profile data with unknown version."));
        return S_FALSE;
    }

    // This module has profile data (this ends up controling the layout of physical and virtual
    // sections within the image, see ZapImage::AllocateVirtualSections.
    m_fHaveProfileData = true;
    m_zapper->m_pOpt->m_fHasAnyProfileData = true;

    CORBBTPROF_SECTION_TABLE_HEADER *sectionHeader;
    READ(sectionHeader, CORBBTPROF_SECTION_TABLE_HEADER);

    //
    // Parse the section table
    //

#ifndef BINDER
    _ASSERTE(TypeProfilingData   == FirstTokenFlagSection + TBL_TypeDef);
    _ASSERTE(MethodProfilingData == FirstTokenFlagSection + TBL_Method);
    _ASSERTE(SectionFormatCount  >= FirstTokenFlagSection + TBL_COUNT + 4);
#endif

    for (ULONG i = 0; i < sectionHeader->NumEntries; i++)
    {
        CORBBTPROF_SECTION_TABLE_ENTRY *entry;
        READ(entry,CORBBTPROF_SECTION_TABLE_ENTRY);

        SectionFormat format = sectionHeader->Entries[i].FormatID;
        if (convertFromV1)
        {
            if (format < LastTokenFlagSection)
            {
                format = (SectionFormat) (format + 1);
            }
        }

        _ASSERTE(format < SectionFormatCount);

        if (format < SectionFormatCount)
        {
            BYTE *start = m_pRawProfileData + sectionHeader->Entries[i].Data.Offset;
            BYTE *end   = start             + sectionHeader->Entries[i].Data.Size;

            if ((start > m_pRawProfileData)                     &&
                (end   < m_pRawProfileData + m_cRawProfileData) &&
                (start < end))
            {
                _ASSERTE(m_profileDataSections[format].pData  == 0);
                _ASSERTE(m_profileDataSections[format].dataSize == 0);

                m_profileDataSections[format].pData     = start;
                m_profileDataSections[format].dataSize  = (DWORD) (end - start);
            }
            else
            {
                _ASSERTE(!"Invalid profile section offset or size");
                return E_FAIL;
            }
        }
    }

    HRESULT hr = S_OK;

    if (convertFromV1)
    {
        hr = convertProfileDataFromV1();
        if (FAILED(hr))
        {
            return hr;
        }
    }
    else if (minified)
    {
        hr = RehydrateProfileData();
        if (FAILED(hr))
        {
            return hr;
        }
    }
    else
    {
        //
        // For those sections that are collections of tokens, further parse that format to get
        // the token pointer and number of tokens
        //

        for (int format = FirstTokenFlagSection; format < SectionFormatCount; format++)
        {
            if (m_profileDataSections[format].pData)
            {
                SEEK(((ULONG) (m_profileDataSections[format].pData - m_pRawProfileData)));

                CORBBTPROF_TOKEN_LIST_SECTION_HEADER *header;
                READ(header, CORBBTPROF_TOKEN_LIST_SECTION_HEADER);

                DWORD tableSize = header->NumTokens;
                DWORD dataSize  = (m_profileDataSections[format].dataSize - sizeof(CORBBTPROF_TOKEN_LIST_SECTION_HEADER));
                DWORD expectedSize = tableSize * sizeof (CORBBTPROF_TOKEN_INFO);

                if (dataSize == expectedSize)
                {
                    BYTE * startOfTable = m_profileDataSections[format].pData + sizeof(CORBBTPROF_TOKEN_LIST_SECTION_HEADER);
                    m_profileDataSections[format].tableSize = tableSize;
                    m_profileDataSections[format].pTable = (CORBBTPROF_TOKEN_INFO *) startOfTable;
                }
                else
                {
                    _ASSERTE(!"Invalid CORBBTPROF_TOKEN_LIST_SECTION_HEADER header");
                    return E_FAIL;
                }
            }
        }
    }

    ZapImage::ProfileDataSection * DataSection_ScenarioInfo = & m_profileDataSections[ScenarioInfo];
    if (DataSection_ScenarioInfo->pData != NULL)
    {
        CORBBTPROF_SCENARIO_INFO_SECTION_HEADER * header = (CORBBTPROF_SCENARIO_INFO_SECTION_HEADER *) DataSection_ScenarioInfo->pData;
        m_profileDataNumRuns = header->TotalNumRuns;
    }

    return S_OK;
}


HRESULT ZapImage::convertProfileDataFromV1()
{
    if (m_pRawProfileData == NULL)
    {
        return S_FALSE;
    }

    //
    // For those sections that are collections of tokens, further parse that format to get
    // the token pointer and number of tokens
    //

    ProfileReader profileReader(m_pRawProfileData, m_cRawProfileData);

    for (SectionFormat format = FirstTokenFlagSection; format < SectionFormatCount; format = (SectionFormat) (format + 1))
    {
        if (m_profileDataSections[format].pData)
        {
            SEEK(((ULONG) (m_profileDataSections[format].pData - m_pRawProfileData)));

            CORBBTPROF_TOKEN_LIST_SECTION_HEADER *header;
            READ(header, CORBBTPROF_TOKEN_LIST_SECTION_HEADER);

            DWORD tableSize = header->NumTokens;

            if (tableSize == 0)
            {
                m_profileDataSections[format].tableSize = 0;
                m_profileDataSections[format].pTable    = NULL;
                continue;
            }

            DWORD dataSize  = (m_profileDataSections[format].dataSize - sizeof(CORBBTPROF_TOKEN_LIST_SECTION_HEADER));
            DWORD expectedSize = tableSize * sizeof (CORBBTPROF_TOKEN_LIST_ENTRY_V1);

            if (dataSize == expectedSize)
            {
                DWORD  newDataSize  = tableSize * sizeof (CORBBTPROF_TOKEN_INFO);

                if (newDataSize < dataSize)
                    return E_FAIL;

                BYTE * startOfTable = new (GetHeap()) BYTE[newDataSize];

                CORBBTPROF_TOKEN_LIST_ENTRY_V1 * pOldEntry;
                CORBBTPROF_TOKEN_INFO *    pNewEntry;

                pOldEntry = (CORBBTPROF_TOKEN_LIST_ENTRY_V1 *) (m_profileDataSections[format].pData + sizeof(CORBBTPROF_TOKEN_LIST_SECTION_HEADER));
                pNewEntry = (CORBBTPROF_TOKEN_INFO *)    startOfTable;

                for (DWORD i=0; i<tableSize; i++)
                {
                    pNewEntry->token = pOldEntry->token;
                    pNewEntry->flags = pOldEntry->flags;
                    pNewEntry->scenarios = 1;

                    pOldEntry++;
                    pNewEntry++;
                }
                m_profileDataSections[format].tableSize = tableSize;
                m_profileDataSections[format].pTable    = (CORBBTPROF_TOKEN_INFO *) startOfTable;
            }
            else
            {
                _ASSERTE(!"Invalid CORBBTPROF_TOKEN_LIST_SECTION_HEADER header");
                return E_FAIL;
            }
        }
    }

    _ASSERTE(m_profileDataSections[ScenarioInfo].pData == 0);
    _ASSERTE(m_profileDataSections[ScenarioInfo].dataSize == 0);

    //
    // Convert the MethodBlockCounts format from V1 to V2
    //
    CORBBTPROF_METHOD_BLOCK_COUNTS_SECTION_HEADER_V1 * mbcSectionHeader = NULL;
    if (m_profileDataSections[MethodBlockCounts].pData)
    {
        //
        // Compute the size of the method block count stream
        // 
        BYTE *  dstPtr           = NULL;
        BYTE *  srcPtr           = m_profileDataSections[MethodBlockCounts].pData;
        DWORD   maxSizeToRead    = m_profileDataSections[MethodBlockCounts].dataSize;
        DWORD   totalSizeNeeded  = 0; 
        DWORD   totalSizeRead    = 0;
       
        mbcSectionHeader = (CORBBTPROF_METHOD_BLOCK_COUNTS_SECTION_HEADER_V1 *) srcPtr;

        totalSizeRead   += sizeof(CORBBTPROF_METHOD_BLOCK_COUNTS_SECTION_HEADER_V1);
        totalSizeNeeded += sizeof(CORBBTPROF_METHOD_BLOCK_COUNTS_SECTION_HEADER); 
        srcPtr          += sizeof(CORBBTPROF_METHOD_BLOCK_COUNTS_SECTION_HEADER_V1);

        if (totalSizeRead > maxSizeToRead)
        {
            return E_FAIL;
        }
       
        for (DWORD i=0; (i < mbcSectionHeader->NumMethods); i++)
        {
            CORBBTPROF_METHOD_HEADER_V1* methodEntry = (CORBBTPROF_METHOD_HEADER_V1 *) srcPtr;
            DWORD sizeRead   = 0;
            DWORD sizeWrite  = 0;

            sizeRead  += methodEntry->HeaderSize;
            sizeRead  += methodEntry->Size;
            sizeWrite += sizeof(CORBBTPROF_METHOD_HEADER);
            sizeWrite += methodEntry->Size;

            totalSizeRead   += sizeRead;
            totalSizeNeeded += sizeWrite;            

            if (totalSizeRead > maxSizeToRead)
            {
                return E_FAIL;
            }

            srcPtr += sizeRead;
        }
        assert(totalSizeRead == maxSizeToRead);

        // Reset the srcPtr
        srcPtr = m_profileDataSections[MethodBlockCounts].pData;
       
        BYTE * newMethodData = new (GetHeap()) BYTE[totalSizeNeeded];

        dstPtr = newMethodData;

        memcpy(dstPtr, srcPtr, sizeof(CORBBTPROF_METHOD_BLOCK_COUNTS_SECTION_HEADER));
        srcPtr += sizeof(CORBBTPROF_METHOD_BLOCK_COUNTS_SECTION_HEADER_V1);
        dstPtr += sizeof(CORBBTPROF_METHOD_BLOCK_COUNTS_SECTION_HEADER);
        
        for (DWORD i=0; (i < mbcSectionHeader->NumMethods); i++)
        {
            CORBBTPROF_METHOD_HEADER_V1 *  methodEntryV1 = (CORBBTPROF_METHOD_HEADER_V1 *) srcPtr;
            CORBBTPROF_METHOD_HEADER *     methodEntry   = (CORBBTPROF_METHOD_HEADER *)    dstPtr;
            DWORD sizeRead   = 0;
            DWORD sizeWrite  = 0;

            methodEntry->method.token   = methodEntryV1->MethodToken;
            methodEntry->method.ILSize  = 0;
            methodEntry->method.cBlock  = (methodEntryV1->Size / sizeof(CORBBTPROF_BLOCK_DATA));
            sizeRead  += methodEntryV1->HeaderSize; 
            sizeWrite += sizeof(CORBBTPROF_METHOD_HEADER);

            memcpy( dstPtr + sizeof(CORBBTPROF_METHOD_HEADER),
                    srcPtr + sizeof(CORBBTPROF_METHOD_HEADER_V1), 
                    (methodEntry->method.cBlock * sizeof(CORBBTPROF_BLOCK_DATA)));
            sizeRead  += methodEntryV1->Size; 
            sizeWrite += (methodEntry->method.cBlock * sizeof(CORBBTPROF_BLOCK_DATA));

            methodEntry->size    = sizeWrite;
            methodEntry->cDetail = 0;
            srcPtr += sizeRead;
            dstPtr += sizeWrite;
        }
       
        m_profileDataSections[MethodBlockCounts].pData    = newMethodData;
        m_profileDataSections[MethodBlockCounts].dataSize = totalSizeNeeded;
    }

    //
    // Allocate the scenario info section
    //
    {
        DWORD   sizeNeeded  = sizeof(CORBBTPROF_SCENARIO_INFO_SECTION_HEADER) + sizeof(CORBBTPROF_SCENARIO_HEADER);
        BYTE *  newData     = new (GetHeap()) BYTE[sizeNeeded];
        BYTE *  dstPtr      = newData;
        {
            CORBBTPROF_SCENARIO_INFO_SECTION_HEADER *siHeader = (CORBBTPROF_SCENARIO_INFO_SECTION_HEADER *) dstPtr;
            
            if (mbcSectionHeader != NULL)
                siHeader->TotalNumRuns = mbcSectionHeader->NumRuns;
            else
                siHeader->TotalNumRuns = 1;

            siHeader->NumScenarios = 1;

            dstPtr += sizeof(CORBBTPROF_SCENARIO_INFO_SECTION_HEADER);
        }
        {
            CORBBTPROF_SCENARIO_HEADER *sHeader = (CORBBTPROF_SCENARIO_HEADER *) dstPtr;

            sHeader->scenario.ordinal  = 1;
            sHeader->scenario.mask     = 1;
            sHeader->scenario.priority = 0;
            sHeader->scenario.numRuns  = 0;
            sHeader->scenario.cName    = 0; 

            sHeader->size = sHeader->Size();

            dstPtr += sizeof(CORBBTPROF_SCENARIO_HEADER);
        }
        m_profileDataSections[ScenarioInfo].pData = newData;
        m_profileDataSections[ScenarioInfo].dataSize = sizeNeeded;
    }

    //
    // Convert the BlobStream format from V1 to V2 
    //   
    if (m_profileDataSections[BlobStream].dataSize > 0)
    {
        //
        // Compute the size of the blob stream
        // 
        
        BYTE *  srcPtr           = m_profileDataSections[BlobStream].pData;
        BYTE *  dstPtr           = NULL;
        DWORD   maxSizeToRead    = m_profileDataSections[BlobStream].dataSize;
        DWORD   totalSizeNeeded  = 0;
        DWORD   totalSizeRead    = 0;
        bool    done             = false;
        
        while (!done)
        {
            CORBBTPROF_BLOB_ENTRY_V1* blobEntry = (CORBBTPROF_BLOB_ENTRY_V1 *) srcPtr;
            DWORD sizeWrite  = 0;
            DWORD sizeRead   = 0;

            if ((blobEntry->blobType >= MetadataStringPool) && (blobEntry->blobType <= MetadataUserStringPool))
            {
                sizeWrite += sizeof(CORBBTPROF_BLOB_POOL_ENTRY);
                sizeWrite += blobEntry->cBuffer;
                sizeRead  += sizeof(CORBBTPROF_BLOB_ENTRY_V1);
                sizeRead  += blobEntry->cBuffer;
            }
            else if ((blobEntry->blobType >= ParamTypeSpec) && (blobEntry->blobType <= ParamMethodSpec))
            {
                sizeWrite += sizeof(CORBBTPROF_BLOB_PARAM_SIG_ENTRY);
                sizeWrite += blobEntry->cBuffer;
                if (blobEntry->blobType == ParamMethodSpec)
                {
                    sizeWrite -= 1;  // Adjust for 
                }
                sizeRead  += sizeof(CORBBTPROF_BLOB_ENTRY_V1);
                sizeRead  += blobEntry->cBuffer;
            }
            else if (blobEntry->blobType == EndOfBlobStream)
            {
                sizeWrite += sizeof(CORBBTPROF_BLOB_ENTRY);
                sizeRead  += sizeof(CORBBTPROF_BLOB_ENTRY_V1);
                done = true;
            }
            else
            {
                return E_FAIL;
            }
            
            totalSizeNeeded += sizeWrite;
            totalSizeRead   += sizeRead;
            
            if (sizeRead > maxSizeToRead)
            {
                return E_FAIL;
            }
            
            srcPtr += sizeRead;
        }

        assert(totalSizeRead == maxSizeToRead);

        // Reset the srcPtr
        srcPtr = m_profileDataSections[BlobStream].pData;
        
        BYTE * newBlobData = new (GetHeap()) BYTE[totalSizeNeeded];

        dstPtr = newBlobData;
        done = false;
        
        while (!done)
        {
            CORBBTPROF_BLOB_ENTRY_V1* blobEntryV1 = (CORBBTPROF_BLOB_ENTRY_V1 *) srcPtr;
            DWORD sizeWrite  = 0;
            DWORD sizeRead   = 0;
            
            if ((blobEntryV1->blobType >= MetadataStringPool) && (blobEntryV1->blobType <= MetadataUserStringPool))
            {
                CORBBTPROF_BLOB_POOL_ENTRY* blobPoolEntry = (CORBBTPROF_BLOB_POOL_ENTRY*) dstPtr;
                
                blobPoolEntry->blob.type = blobEntryV1->blobType;
                blobPoolEntry->blob.size = sizeof(CORBBTPROF_BLOB_POOL_ENTRY) + blobEntryV1->cBuffer;
                blobPoolEntry->cBuffer   = blobEntryV1->cBuffer;
                memcpy(blobPoolEntry->buffer, blobEntryV1->pBuffer, blobEntryV1->cBuffer);
                
                sizeWrite += sizeof(CORBBTPROF_BLOB_POOL_ENTRY);
                sizeWrite += blobEntryV1->cBuffer;
                sizeRead  += sizeof(CORBBTPROF_BLOB_ENTRY_V1);
                sizeRead  += blobEntryV1->cBuffer;
            }
            else if ((blobEntryV1->blobType >= ParamTypeSpec) && (blobEntryV1->blobType <= ParamMethodSpec))
            {
                CORBBTPROF_BLOB_PARAM_SIG_ENTRY* blobSigEntry = (CORBBTPROF_BLOB_PARAM_SIG_ENTRY*) dstPtr;

                blobSigEntry->blob.type  = blobEntryV1->blobType;
                blobSigEntry->blob.size  = sizeof(CORBBTPROF_BLOB_PARAM_SIG_ENTRY) + blobEntryV1->cBuffer;
                blobSigEntry->blob.token = 0;
                blobSigEntry->cSig       = blobEntryV1->cBuffer; 

                if (blobEntryV1->blobType == ParamMethodSpec)
                {
                    // Adjust cSig and blob.size
                    blobSigEntry->cSig--; 
                    blobSigEntry->blob.size--;
                }
                memcpy(blobSigEntry->sig, blobEntryV1->pBuffer, blobSigEntry->cSig);
                
                sizeWrite += sizeof(CORBBTPROF_BLOB_PARAM_SIG_ENTRY);
                sizeWrite += blobSigEntry->cSig;
                sizeRead  += sizeof(CORBBTPROF_BLOB_ENTRY_V1);
                sizeRead  += blobEntryV1->cBuffer;
            }
            else if (blobEntryV1->blobType == EndOfBlobStream)
            {
                CORBBTPROF_BLOB_ENTRY* blobEntry = (CORBBTPROF_BLOB_ENTRY*) dstPtr;

                blobEntry->type = blobEntryV1->blobType;
                blobEntry->size = sizeof(CORBBTPROF_BLOB_ENTRY);
                
                sizeWrite += sizeof(CORBBTPROF_BLOB_ENTRY);
                sizeRead  += sizeof(CORBBTPROF_BLOB_ENTRY_V1);
                done = true;
            }
            else
            {
                return E_FAIL;
            }
            srcPtr += sizeRead;
            dstPtr += sizeWrite;
        }
       
        m_profileDataSections[BlobStream].pData    = newBlobData;
        m_profileDataSections[BlobStream].dataSize = totalSizeNeeded;
    }
    else
    {
        m_profileDataSections[BlobStream].pData    = NULL;
        m_profileDataSections[BlobStream].dataSize = 0;
    }

    return S_OK;
}

void ZapImage::RehydrateBasicBlockSection()
{
    ProfileDataSection &section = m_profileDataSections[MethodBlockCounts];
    if (!section.pData)
    {
        return;
    }

    ProfileReader reader(section.pData, section.dataSize);

    m_profileDataNumRuns = reader.Read<unsigned int>();

    // The IBC data provides a hint to the number of basic blocks, which is
    // used here to determine how much space to allocate for the rehydrated
    // data.
    unsigned int blockCountHint = reader.Read<unsigned int>();

    unsigned int numMethods = reader.Read<unsigned int>();

    unsigned int expectedLength =
        sizeof(CORBBTPROF_METHOD_BLOCK_COUNTS_SECTION_HEADER) +
        sizeof(CORBBTPROF_METHOD_HEADER) * numMethods +
        sizeof(CORBBTPROF_BLOCK_DATA) * blockCountHint;

    BinaryWriter writer(expectedLength, GetHeap());

    writer.Write(numMethods);

    mdToken lastMethodToken = 0x06000000;

    CORBBTPROF_METHOD_HEADER methodHeader;
    methodHeader.cDetail = 0;
    methodHeader.method.ILSize = 0;

    for (unsigned int i = 0; i < numMethods; ++i)
    {
        // Translate the method header
        unsigned int size = reader.Read7BitEncodedInt();
        unsigned int startPosition = reader.GetCurrentPos();

        mdToken token = reader.ReadTokenWithMemory(lastMethodToken);
        unsigned int ilSize = reader.Read7BitEncodedInt();
        unsigned int firstBlockHitCount = reader.Read7BitEncodedInt();

        unsigned int numOtherBlocks = reader.Read7BitEncodedInt();

        methodHeader.method.cBlock = 1 + numOtherBlocks;
        methodHeader.method.token = token;
        methodHeader.method.ILSize = ilSize;
        methodHeader.size = (DWORD)methodHeader.Size();

        writer.Write(methodHeader);

        CORBBTPROF_BLOCK_DATA blockData;

        // The first block is handled specially.
        blockData.ILOffset = 0;
        blockData.ExecutionCount = firstBlockHitCount;

        writer.Write(blockData);

        // Translate the rest of the basic blocks
        for (unsigned int j = 0; j < numOtherBlocks; ++j)
        {
            blockData.ILOffset = reader.Read7BitEncodedInt();
            blockData.ExecutionCount = reader.Read7BitEncodedInt();

            writer.Write(blockData);
        }

        if (!reader.Seek(startPosition + size))
        {
            ThrowHR(E_FAIL);
        }
    }

    // If the expected and actual lengths differ, the result will still be
    // correct but performance may suffer slightly because of reallocations.
    _ASSERTE(writer.GetWrittenSize() == expectedLength);

    section.pData = writer.GetBuffer();
    section.dataSize = writer.GetWrittenSize();
}

void ZapImage::RehydrateTokenSection(int sectionFormat, unsigned int flagTable[255])
{
    ProfileDataSection &section = m_profileDataSections[sectionFormat];
    ProfileReader reader(section.pData, section.dataSize);

    unsigned int numTokens = reader.Read<unsigned int>();

    unsigned int dataLength = sizeof(unsigned int) +
                              numTokens * sizeof(CORBBTPROF_TOKEN_INFO);
    BinaryWriter writer(dataLength, GetHeap());

    writer.Write(numTokens);

    mdToken lastToken = (sectionFormat - FirstTokenFlagSection) << 24;

    CORBBTPROF_TOKEN_INFO tokenInfo;
    tokenInfo.scenarios = 1;

    for (unsigned int i = 0; i < numTokens; ++i)
    {
        tokenInfo.token = reader.ReadTokenWithMemory(lastToken);
        tokenInfo.flags = reader.ReadFlagWithLookup(flagTable);

        writer.Write(tokenInfo);
    }

    _ASSERTE(writer.GetWrittenSize() == dataLength);
    
    section.pData = writer.GetBuffer();
    section.dataSize = writer.GetWrittenSize();
    section.pTable = (CORBBTPROF_TOKEN_INFO *)(section.pData + sizeof(unsigned int));
    section.tableSize = numTokens;
}

void ZapImage::RehydrateBlobStream()
{
    ProfileDataSection &section = m_profileDataSections[BlobStream];

    ProfileReader reader(section.pData, section.dataSize);

    // Evidence suggests that rehydrating the blob stream in Framework binaries
    // increases the size from 1.5-2x. When this was written, 1.85x minimized
    // the amount of extra memory allocated (about 48K in the worst case).
    BinaryWriter writer((DWORD)(section.dataSize * 1.85f), GetHeap());

    mdToken LastBlobToken = 0;
    mdToken LastAssemblyToken = 0x23000000;
    mdToken LastExternalTypeToken = 0x62000000;
    mdToken LastExternalNamespaceToken = 0x61000000;
    mdToken LastExternalSignatureToken = 0x63000000;

    int blobType = 0;
    do
    {
        // Read the blob header.

        unsigned int sizeToRead = reader.Read7BitEncodedInt();
        unsigned int startPositionRead = reader.GetCurrentPos();
    
        blobType = reader.Read7BitEncodedInt();
        mdToken token = reader.ReadTokenWithMemory(LastBlobToken);

        // Write out the blob header.

        // Note the location in the write stream, and write a 0 there. Once
        // this blob has been written in its entirety, this location can be
        // used to calculate the real size and to go back to the right place
        // to write it.

        unsigned int startPositionWrite = writer.GetWrittenSize();
        writer.Write(0U);

        writer.Write(blobType);
        writer.Write(token);

        // All blobs (except the end-of-stream indicator) end as:
        //     <data length> <data>
        // Two blob types (handled immediately below) include tokens as well.
        // Handle those first, then handle the common case.

        if (blobType == ExternalTypeDef)
        {
            writer.Write(reader.ReadTokenWithMemory(LastAssemblyToken));
            writer.Write(reader.ReadTokenWithMemory(LastExternalTypeToken));
            writer.Write(reader.ReadTokenWithMemory(LastExternalNamespaceToken));
        }
        else if (blobType == ExternalMethodDef)
        {
            writer.Write(reader.ReadTokenWithMemory(LastExternalTypeToken));
            writer.Write(reader.ReadTokenWithMemory(LastExternalSignatureToken));
        }

        if ((blobType >= MetadataStringPool) && (blobType < IllegalBlob))
        {
            // This blob is of known type and ends with data.
            unsigned int dataLength = reader.Read7BitEncodedInt();
            char *data = (char *)reader.Read(dataLength);

            if (!data)
            {
                ThrowHR(E_FAIL);
            }

            writer.Write(dataLength);
            writer.Write(data, dataLength);
        }

        // Write the size for this blob.

        writer.WriteAt(startPositionWrite,
                       writer.GetWrittenSize() - startPositionWrite);

        // Move to the next blob.

        if (!reader.Seek(startPositionRead + sizeToRead))
        {
            ThrowHR(E_FAIL);
        }
    }
    while (blobType != EndOfBlobStream);

    section.pData = writer.GetBuffer();
    section.dataSize = writer.GetWrittenSize();
}

HRESULT ZapImage::RehydrateProfileData()
{
    HRESULT hr = S_OK;
    unsigned int flagTable[255];
    memset(flagTable, 0xFF, sizeof(flagTable));
    
    EX_TRY
    {
        RehydrateBasicBlockSection();
        RehydrateBlobStream();
        for (int format = FirstTokenFlagSection;
             format < SectionFormatCount;
             ++format)
        {
            if (m_profileDataSections[format].pData)
            {
                RehydrateTokenSection(format, flagTable);
            }
        }
    }
    EX_CATCH_HRESULT_NO_ERRORINFO(hr);

    return hr;
}

HRESULT ZapImage::hashBBProfileData ()
{
    ProfileDataSection * DataSection_MethodBlockCounts = & m_profileDataSections[MethodBlockCounts];

    if (!DataSection_MethodBlockCounts->pData)
    {
        return E_FAIL;
    }

    ProfileReader profileReader(DataSection_MethodBlockCounts->pData, DataSection_MethodBlockCounts->dataSize);

    CORBBTPROF_METHOD_BLOCK_COUNTS_SECTION_HEADER *mbcHeader;
    READ(mbcHeader,CORBBTPROF_METHOD_BLOCK_COUNTS_SECTION_HEADER);

    for (ULONG i = 0; i < mbcHeader->NumMethods; i++)
    {
        ProfileDataHashEntry newEntry;
        newEntry.pos = profileReader.GetCurrentPos();
        
        CORBBTPROF_METHOD_HEADER *methodHeader;
        READ(methodHeader,CORBBTPROF_METHOD_HEADER);
        newEntry.md   = methodHeader->method.token;
        newEntry.size = methodHeader->size;

        // Add the new entry to the table
        profileDataHashTable.Add(newEntry);

        // Skip the profileData so we can read the next method.
        void *profileData;
        READ_SIZE(profileData, void, (methodHeader->size - sizeof(CORBBTPROF_METHOD_HEADER)));
    }

    return S_OK;
}

void ZapImage::LoadProfileData()
{
    HRESULT hr = E_FAIL;

    m_fHaveProfileData = false;
    m_pRawProfileData  = NULL;
    m_cRawProfileData  = 0;

    EX_TRY
    {
        hr = LocateProfileData();
        
        if (hr == S_OK)
        {
            hr = parseProfileData();
            if (hr == S_OK)
            {
                hr = hashBBProfileData();
            }
        }
    }
    EX_CATCH
    {
        hr = E_FAIL;
    }
    EX_END_CATCH(SwallowAllExceptions);
    
    if (hr != S_OK)
    {
        m_fHaveProfileData = false;
        m_pRawProfileData = NULL;
        m_cRawProfileData = 0;

        if (FAILED(hr))
        {
            m_zapper->Warning(W("Warning: Invalid profile data was ignored for %s\n"), m_pModuleFileName);
        }
    }
}

// Initializes our form of the profile data stored in the assembly.

CorProfileData *  ZapImage::NewProfileData()
{
    this->m_pCorProfileData = new CorProfileData(&m_profileDataSections[0]);

    return this->m_pCorProfileData;
}

// Returns the profile data stored in the assembly.

CorProfileData *  ZapImage::GetProfileData()
{
    _ASSERTE(this->m_pCorProfileData != NULL);

    return this->m_pCorProfileData;
}

CorProfileData::CorProfileData(void *  rawProfileData)
{
    ZapImage::ProfileDataSection * profileData =  (ZapImage::ProfileDataSection *) rawProfileData;

    for (DWORD format = 0; format < SectionFormatCount; format++)
    {
        this->profilingTokenFlagsData[format].count = profileData[format].tableSize;
        this->profilingTokenFlagsData[format].data  = profileData[format].pTable;
    }

    this->blobStream = (CORBBTPROF_BLOB_ENTRY *) profileData[BlobStream].pData;
}


// Determines whether a method can be called directly from another method (without
// going through the prestub) in the current module.
// callerFtn=NULL implies any/unspecified caller in the current module.
//
// Returns NULL if 'calleeFtn' cannot be called directly *at the current time*
// Else returns the direct address that 'calleeFtn' can be called at.


bool ZapImage::canIntraModuleDirectCall(
                        CORINFO_METHOD_HANDLE callerFtn,
                        CORINFO_METHOD_HANDLE targetFtn,
                        CorInfoIndirectCallReason *pReason,
                        CORINFO_ACCESS_FLAGS  accessFlags/*=CORINFO_ACCESS_ANY*/)
{
    CorInfoIndirectCallReason reason;
    if (pReason == NULL)
        pReason = &reason;
    *pReason = CORINFO_INDIRECT_CALL_UNKNOWN;

    // The caller should have checked that the method is in current loader module
    _ASSERTE(m_hModule == m_zapper->m_pEECompileInfo->GetLoaderModuleForEmbeddableMethod(targetFtn));

    // No direct calls at all under some circumstances

    if ((m_zapper->m_pOpt->m_compilerFlags & CORJIT_FLG_PROF_ENTERLEAVE)
        && !m_pPreloader->IsDynamicMethod(callerFtn))
    {
        *pReason = CORINFO_INDIRECT_CALL_PROFILING;
        goto CALL_VIA_ENTRY_POINT;
    }

    // Does the methods's class have a cctor, etc?

    if (!m_pPreloader->CanSkipMethodPreparation(callerFtn, targetFtn, pReason, accessFlags))
        goto CALL_VIA_ENTRY_POINT;

    ZapMethodHeader * pMethod;
    pMethod = GetCompiledMethod(targetFtn);

    // If we have not compiled the method then we can't call direct

    if (pMethod == NULL)
    {
        *pReason = CORINFO_INDIRECT_CALL_NO_CODE;
        goto CALL_VIA_ENTRY_POINT;
    }

    // Does the method have fixups?

    if (pMethod->HasFixups() != NULL)
    {
        *pReason = CORINFO_INDIRECT_CALL_FIXUPS;
        goto CALL_VIA_ENTRY_POINT;
    }

#ifdef _DEBUG
    const char* clsName, * methodName;
    LOG((LF_ZAP, LL_INFO10000, "getIntraModuleDirectCallAddr: Success %s::%s\n",
        clsName, (methodName = m_zapper->m_pEEJitInfo->getMethodName(targetFtn, &clsName), methodName)));
#endif

    return true;

CALL_VIA_ENTRY_POINT:

#ifdef _DEBUG
    LOG((LF_ZAP, LL_INFO10000, "getIntraModuleDirectCallAddr: Via EntryPoint %s::%s\n",
         clsName, (methodName = m_zapper->m_pEEJitInfo->getMethodName(targetFtn, &clsName), methodName)));
#endif

    return false;
}

//
// Relocations
//

void ZapImage::WriteReloc(PVOID pSrc, int offset, ZapNode * pTarget, int targetOffset, ZapRelocationType type)
{
    _ASSERTE(!IsWritingRelocs());

    _ASSERTE(m_pBaseRelocs != NULL);
    m_pBaseRelocs->WriteReloc(pSrc, offset, pTarget, targetOffset, type);
}

ZapImage * ZapImage::GetZapImage()
{
    return this;
}

#ifndef BINDER
void ZapImage::FileNotFoundError(LPCWSTR pszMessage)
{
    SString message(pszMessage);

    for (COUNT_T i = 0; i < fileNotFoundErrorsTable.GetCount(); i++)
    {
        // Check to see if same error has already been displayed for this ngen operation
        if (message.Equals(fileNotFoundErrorsTable[i]))
            return;
    }

    CorZapLogLevel level;

#ifdef CROSSGEN_COMPILE
    // Warnings should not go to stderr during crossgen
    level = CORZAP_LOGLEVEL_WARNING;
#else
    level = CORZAP_LOGLEVEL_ERROR;
#endif

#ifndef FEATURE_CORECLR
    m_zapper->Print(level, W("Warning: %s. If this assembly is found during runtime of an application, then the native image currently being generated will not be used.\n"), pszMessage);
#else
    m_zapper->Print(level, W("Warning: %s.\n"), pszMessage);
#endif

    fileNotFoundErrorsTable.Append(message);
}
#endif

void ZapImage::Error(mdToken token, HRESULT hr, LPCWSTR message)
{
#if defined(FEATURE_CORECLR) || defined(CROSSGEN_COMPILE)
    // Missing dependencies are reported as fatal errors in code:CompilationDomain::BindAssemblySpec.
    // Avoid printing redundant error message for them.
    if (FAILED(g_hrFatalError))
        ThrowHR(g_hrFatalError);
#endif

    CorZapLogLevel level = CORZAP_LOGLEVEL_ERROR;

#ifndef BINDER
    if (RuntimeFileNotFound(hr) || (hr == CORSEC_E_INVALID_STRONGNAME))
    {
        // FileNotFound errors here can be converted into a single error string per ngen compile, 
        // and the detailed error is available with verbose logging
        if (m_zapper->m_pOpt->m_ignoreErrors && message != NULL)
        {
            FileNotFoundError(message);
            level = CORZAP_LOGLEVEL_INFO;
         }
    }
#endif

    if (m_zapper->m_pOpt->m_ignoreErrors)
    {
#ifdef CROSSGEN_COMPILE
        // Warnings should not go to stderr during crossgen
        if (level == CORZAP_LOGLEVEL_ERROR)
            level = CORZAP_LOGLEVEL_WARNING;
#endif
        m_zapper->Print(level, W("Warning: "));
    }
    else
    {
        m_zapper->Print(level, W("Error: "));
    }

    if (message != NULL)
        m_zapper->Print(level, W("%s"), message);
    else
        m_zapper->PrintErrorMessage(level, hr);

    m_zapper->Print(level, W(" while resolving 0x%x - "), token);
    PrintTokenDescription(level, token);
    m_zapper->Print(level, W(".\n"));

    if (m_zapper->m_pOpt->m_ignoreErrors)
        return;

    IfFailThrow(hr);
}

ZapNode * ZapImage::GetInnerPtr(ZapNode * pNode, SSIZE_T offset)
{
    return m_pInnerPtrs->Get(pNode, offset);
}

ZapNode * ZapImage::GetHelperThunk(CorInfoHelpFunc ftnNum)
{
    ZapNode * pHelperThunk = m_pHelperThunks[ftnNum];

    if (pHelperThunk == NULL)
    {
        pHelperThunk = new (GetHeap()) ZapHelperThunk(ftnNum);
#ifndef BINDER
#ifdef _TARGET_ARM_
        pHelperThunk = GetInnerPtr(pHelperThunk, THUMB_CODE);
#endif
#endif // !BINDER
        m_pHelperThunks[ftnNum] = pHelperThunk;
    }

    // Ensure that the thunk is placed
    ZapNode * pTarget = pHelperThunk;
    if (pTarget->GetType() == ZapNodeType_InnerPtr)
        pTarget = ((ZapInnerPtr *)pTarget)->GetBase();
    if (!pTarget->IsPlaced())
        m_pHelperTableSection->Place(pTarget);

    return pHelperThunk;
}

//
// Compute a class-layout order based on a breadth-first traversal of 
// the class graph (based on what classes contain calls to other classes).
// We cannot afford time or space to build the graph, so we do processing
// in place.
// 
void ZapImage::ComputeClassLayoutOrder()
{
    // In order to make the computation efficient, we need to store per-class 
    // intermediate values in the class layout field.  These come in two forms:
    // 
    //   - An entry with the UNSEEN_CLASS_FLAG set is one that is yet to be encountered.
    //   - An entry with METHOD_INDEX_FLAG set is an index into the m_MethodCompilationOrder list
    //     indicating where the unprofiled methods of this class begin
    //   
    // Both flags begin set (by InitializeClassLayoutOrder) since the value initialized is
    // the method index and the class has not been encountered by the algorithm.
    // When a class layout has been computed, both of these flags will have been stripped.


    // Early-out in the (probably impossible) case that these bits weren't available
    if (m_MethodCompilationOrder.GetCount() >= UNSEEN_CLASS_FLAG ||
        m_MethodCompilationOrder.GetCount() >= METHOD_INDEX_FLAG)
    {
        return;
    }

    // Allocate the queue for the breadth-first traversal.
    // Note that the use of UNSEEN_CLASS_FLAG ensures that no class is enqueued more
    // than once, so we can use that bound for the size of the queue.
    CORINFO_CLASS_HANDLE * classQueue = new CORINFO_CLASS_HANDLE[m_ClassLayoutOrder.GetCount()];

    unsigned classOrder = 0;
    for (COUNT_T i = m_iUntrainedMethod; i < m_MethodCompilationOrder.GetCount(); i++)
    {
        unsigned classQueueNext = 0;
        unsigned classQueueEnd = 0;
        COUNT_T  methodIndex = 0;

        //
        // Find an unprocessed method to seed the next breadth-first traversal.
        //

        ZapMethodHeader * pMethod = m_MethodCompilationOrder[i];
        const ClassLayoutOrderEntry * pEntry = m_ClassLayoutOrder.LookupPtr(pMethod->m_classHandle);
        _ASSERTE(pEntry);
        
        if ((pEntry->m_order & UNSEEN_CLASS_FLAG) == 0)
        {
            continue;
        }

        //
        // Enqueue the method's class and start the traversal.
        //

        classQueue[classQueueEnd++] = pMethod->m_classHandle;
        ((ClassLayoutOrderEntry *)pEntry)->m_order &= ~UNSEEN_CLASS_FLAG;

        while (classQueueNext < classQueueEnd)
        {
            //
            // Dequeue a class and pull out the index of its first method
            //
            
            CORINFO_CLASS_HANDLE dequeuedClassHandle = classQueue[classQueueNext++];
            _ASSERTE(dequeuedClassHandle != NULL);

            pEntry = m_ClassLayoutOrder.LookupPtr(dequeuedClassHandle);
            _ASSERTE(pEntry);
            _ASSERTE((pEntry->m_order & UNSEEN_CLASS_FLAG) == 0);
            _ASSERTE((pEntry->m_order & METHOD_INDEX_FLAG) != 0);

            methodIndex = pEntry->m_order & ~METHOD_INDEX_FLAG;
            _ASSERTE(methodIndex < m_MethodCompilationOrder.GetCount());

            //
            // Set the real layout order of the class, and examine its unprofiled methods
            //
            
            ((ClassLayoutOrderEntry *)pEntry)->m_order = ++classOrder;
                
            pMethod = m_MethodCompilationOrder[methodIndex];
            _ASSERTE(pMethod->m_classHandle == dequeuedClassHandle);

            while (pMethod->m_classHandle == dequeuedClassHandle)
            {

                //
                // For each unprofiled method, find target classes and enqueue any that haven't been seen
                //

                ZapMethodHeader::PartialTargetMethodIterator it(pMethod);

                CORINFO_METHOD_HANDLE targetMethodHandle;
                while (it.GetNext(&targetMethodHandle))
                {
                    CORINFO_CLASS_HANDLE targetClassHandle = GetJitInfo()->getMethodClass(targetMethodHandle);
                    if (targetClassHandle != pMethod->m_classHandle)
                    {
                        pEntry = m_ClassLayoutOrder.LookupPtr(targetClassHandle);

                        if (pEntry && (pEntry->m_order & UNSEEN_CLASS_FLAG) != 0)
                        {
                            _ASSERTE(classQueueEnd < m_ClassLayoutOrder.GetCount());
                            classQueue[classQueueEnd++] = targetClassHandle;

                            ((ClassLayoutOrderEntry *)pEntry)->m_order &= ~UNSEEN_CLASS_FLAG;
                        }
                    }
                }

                if (++methodIndex == m_MethodCompilationOrder.GetCount())
                {
                    break;
                }
                    
                pMethod = m_MethodCompilationOrder[methodIndex];
            }
        }
    }

    for (COUNT_T i = m_iUntrainedMethod; i < m_MethodCompilationOrder.GetCount(); i++)
    {
        ZapMethodHeader * pMethod = m_MethodCompilationOrder[i];
        pMethod->m_cachedLayoutOrder = LookupClassLayoutOrder(pMethod->m_classHandle);
    }

    m_fHasClassLayoutOrder = true;

    delete [] classQueue;
}

static int __cdecl LayoutOrderCmp(const void* a_, const void* b_)
{
    ZapMethodHeader * a = *((ZapMethodHeader**)a_);
    ZapMethodHeader * b = *((ZapMethodHeader**)b_);

    int layoutDiff = a->GetCachedLayoutOrder() - b->GetCachedLayoutOrder();
    if (layoutDiff != 0)
        return layoutDiff;

    // Use compilation order as secondary key to get predictable ordering within the bucket
    return a->GetCompilationOrder() - b->GetCompilationOrder();
}

void ZapImage::SortUnprofiledMethodsByClassLayoutOrder()
{
    qsort(&m_MethodCompilationOrder[m_iUntrainedMethod], m_MethodCompilationOrder.GetCount() - m_iUntrainedMethod, sizeof(ZapMethodHeader *), LayoutOrderCmp);
}
