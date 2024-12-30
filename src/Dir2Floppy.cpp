
#include <windows.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <io.h>
#include "Dir2Floppy.h"

#include "zip/zipio.h"

//--------------- Disk geometry ----------------------------------------
static	const	int		NB_SECTOR_PER_TRACK	=	10;
static	const	int		NB_CYLINDER			=	81;
static	const	int		NB_HEAD				=	2;

//--------------- Filesystem geometry ----------------------------------
static	const	int		MAX_ROOT_ENTRY		=	112;
static	const	int		SECTOR_PER_FAT		=	5;
static	const	int		ROOTDIR_NBSECTOR	=	(MAX_ROOT_ENTRY*32)/512;




CFloppy::CFloppy()
{
	m_pRawImage = NULL;
	m_pFat = NULL;
}

CFloppy::~CFloppy()
{
	Destroy();
}

void	CFloppy::Destroy()
{
	if (m_pRawImage)
	{
		delete [] m_pRawImage;
		m_pRawImage = NULL;
	}

	if (m_pFat)
	{
		delete [] m_pFat;
		m_pFat = NULL;
	}
}



bool	CFloppy::Create(int nbSide,int nbSectorPerTrack,int nbCylinder)
{

	Destroy();

	m_nbSide = nbSide;
	m_nbCylinder = nbCylinder;
	m_nbSectorPerTrack = nbSectorPerTrack;
	m_rawSize = nbSide * nbSectorPerTrack * nbCylinder * 512;

	m_pRawImage = new unsigned char [m_rawSize];

	if (m_pRawImage)
	{
		// Fill the raw image
		memset(m_pRawImage,0xe5,m_rawSize);

		// Build the bootsector
		w16(0xb,512);				// byte per sector
		w8(0xd,2);					// sector per cluster
		w16(0xe,1);					// reserved sector (boot sector)
		w8(0x10,2);					// number of fat !! (2 fat per disk)
		w16(0x11,MAX_ROOT_ENTRY);	// Nb root entries
		w16(0x13,nbSide * nbSectorPerTrack * nbCylinder);	// total sectors
		w8(0x15,0xf7);				// media type
		w16(0x16,SECTOR_PER_FAT);	// sectors per fat
		w16(0x18,m_nbSectorPerTrack);
		w16(0x1a,m_nbSide);

		// atari specific
		w16(0x0,0xe9);
		w16(0x1c,0);
		memset(m_pRawImage + 0x1e,0x4e,30);

		int nbFsSector = 1 + SECTOR_PER_FAT * 2 + ROOTDIR_NBSECTOR;
		int nbDataSector = nbSide * nbSectorPerTrack * nbCylinder - nbFsSector;
		m_maxFatEntry = (nbDataSector / 2);
		m_nbFreeCluster = m_maxFatEntry;
		m_nextCluster = 2;
		m_pFat = new int [m_maxFatEntry];
		memset(m_pFat,0,m_maxFatEntry * sizeof(int));



	}

	return (NULL != m_pRawImage);
}

CDirEntry::CDirEntry()
{
	m_pNext = NULL;
	m_pDirectory = NULL;
	m_pFileData = NULL;
}

CDirEntry::~CDirEntry()
{
	if (m_pDirectory)
		delete m_pDirectory;
	
}

CDirectory::CDirectory()
{
	m_nbEntry = 0;
	m_pEntryList = NULL;
}

CDirectory::~CDirectory()
{
	
	CDirEntry *pEntry = m_pEntryList;
	while (pEntry)
	{
		CDirEntry *pTmp = pEntry;
		pEntry = pEntry->GetNext();
		delete pTmp;
	}
}


void	CDirEntry::Create(const FileDescriptor *pInfo,CDirectory *pDir,const char *pHostName, ZFILE* pZIP)
{
	m_info = *pInfo;
	m_pDirectory = pDir;

	if ( pHostName )
	{
		strcpy(m_sHostName,pHostName);
		FILE* h = fopen( m_sHostName, "rb" );
		if ( h )
		{
			m_pFileData = malloc( m_info.nFileSizeLow + 1 );	// +1 to avoid problem with 0 bytes file
			fread( m_pFileData, 1, m_info.nFileSizeLow, h );
			fclose( h );
		}
		else
		{
			printf("FATAL ERROR: Could not load \"%s\"\n", m_info.cFileName );
		}
	}

	if ( pZIP )
	{
		zseek( pZIP, 0, SEEK_SET);		// ZLIB bug !! Must seek to 0 before to seek end, error if not !
		zseek( pZIP, 0, SEEK_END);
		m_info.nFileSizeLow = ztell( pZIP );
		zseek( pZIP, 0, SEEK_SET);		// ZLIB bug !! Must seek to 0 before to seek end, error if not !
		m_pFileData = malloc( m_info.nFileSizeLow + 1 );	// +1 to avoid problem with 0 bytes file
		zread( m_pFileData, 1, m_info.nFileSizeLow, pZIP );
	}
}

void	CDirectory::AddEntry(const FileDescriptor *pInfo,CDirectory *pSubDir,const char *pHostName, ZFILE* pZIP)
{
	CDirEntry *pEntry = new CDirEntry;
	pEntry->Create(pInfo,pSubDir,pHostName, pZIP);
	pEntry->SetNext(m_pEntryList);
	m_pEntryList = pEntry;
	m_nbEntry++;
}


void	DirectoryScan(const char *pDir,CDirectory *pCurrent)
{

	char tmpName[_MAX_PATH];
	strcpy(tmpName,pDir);
	strcat(tmpName,"\\*.*");
	
	FileDescriptor info;
	HANDLE hSearch = FindFirstFile(tmpName,&info);
	if (hSearch != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (0 == (info.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM)))
			{
				strcpy(tmpName,pDir);
				sprintf(tmpName,"%s\\%s",pDir,info.cFileName);

				if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				{
					if ('.' != info.cFileName[0])		// skip original "." and ".." entries on host system
					{
						CDirectory *pNewDir = new CDirectory;
						pCurrent->AddEntry(&info,pNewDir,tmpName);
						DirectoryScan(tmpName,pNewDir);
					}
				}
				else
				{
					pCurrent->AddEntry(&info,NULL,tmpName);
				}
			}
		}
		while(FindNextFile(hSearch,&info));

		FindClose(hSearch);
	}
}


void	CDirectory::Dump(const char *pPath)
{

	char sTmp[_MAX_PATH];

	printf("[%s] ( %d entries )\n",pPath,m_nbEntry);

	CDirEntry *pEntry = m_pEntryList;
	while (pEntry)
	{
		if (pEntry->GetDirectory())
		{
			sprintf(sTmp,"%s/%s",pPath,pEntry->GetName());
			pEntry->GetDirectory()->Dump(sTmp);
		}
		pEntry = pEntry->GetNext();
	}

	pEntry = m_pEntryList;
	while (pEntry)
	{
		if (!pEntry->GetDirectory())
		{
			printf("  %10d : %s\n",pEntry->GetSize(),pEntry->GetName());
		}
		pEntry = pEntry->GetNext();
	}

	printf("\n");
}

CDirectory	*	CreateTreeFromDirectory(const char *pHostDirName)
{

	CDirectory *pRoot = new CDirectory();

	DirectoryScan(pHostDirName,pRoot);

	return pRoot;
}


const char* DirAdvance( const char* pCurrent, char* pOut )
{
	for (;;)
	{

		if ( '/' == *pCurrent )
		{
			*pOut++ = 0;
			return pCurrent+1;
		}
		else
		{
			*pOut++ = *pCurrent++;
			if ( pOut[-1] == 0 )
				return pCurrent-1;
		}
	}
}


CDirectory*	GetFromZIPPath( CDirectory* pRoot, const char* pZIPPath )
{
	const char* pParse = pZIPPath;
	char sDirName[ _MAX_PATH ];

	CDirectory* pCurrent = pRoot;

	for (;;)
	{

		assert( *pParse );								// if this is the end, we miss the filename
		pParse = DirAdvance( pParse, sDirName );

		if ( 0 == *pParse )
			break;					// last name is the filename (not a dir name)

		// maybe directory already exist
		CDirEntry* pEntry = pCurrent->GetFirstEntry();
		CDirEntry* pFound = NULL;
		while ( pEntry )
		{
			if ( pEntry->IsDirectory() )
			{
				if ( 0 == stricmp( pEntry->GetName(), sDirName ) )
				{	// create a new one
					pFound = pEntry;
					break;
				}
			}
			pEntry = pEntry->GetNext();
		}

		if ( NULL == pFound )
		{
			printf("INTERNAL Error GetFromZIPPath!\nAbort\n" );
			exit(1);
		}

		pCurrent = pFound->GetDirectory();

	}
	return pCurrent;
}


void	CreateDirPath( CDirectory* pRoot, const char* sZIPPath )
{

	const char* pParse = sZIPPath;
	char sDirName[ _MAX_PATH ];

	CDirectory* pCurrent = pRoot;

	while ( *pParse )
	{
		pParse = DirAdvance( pParse, sDirName );

		// maybe directory already exist
		CDirEntry* pEntry = pCurrent->GetFirstEntry();
		CDirEntry* pFound = NULL;
		while ( pEntry )
		{
			if ( pEntry->IsDirectory() )
			{
				if ( 0 == stricmp( pEntry->GetName(), sDirName ) )
				{	// create a new one
					pFound = pEntry;
					break;
				}
			}
			pEntry = pEntry->GetNext();
		}

		if ( NULL == pFound )
		{
			FileDescriptor oFDesc;
			memset( &oFDesc, 0, sizeof( oFDesc ) );

			strncpy( oFDesc.cFileName, sDirName, 13 );

			CDirectory *pNewDir = new CDirectory;

			pCurrent->AddEntry( &oFDesc, pNewDir, NULL );

			pCurrent = pNewDir;

		}
		else
			pCurrent = pFound->GetDirectory();

	}

}


CDirectory* CreateTreeFromZIP( const char *pHostDirName, ZFILE* pFile )
{
	CDirectory *pRoot = new CDirectory();


	for (;;)
	{
		const char* pPath = zname( pFile );
		if ( NULL == pPath )
			break;

		int iLen = strlen( pPath );
		if ( iLen > 0)
		{
			if ( '/' == pPath[ iLen-1 ] )
			{	// new directory
				// create complete path from root
				CreateDirPath( pRoot, pPath );
			}
			else
			{	// supposed to be a file
				CDirectory* pDir = GetFromZIPPath( pRoot, pPath );

				FileDescriptor oFDesc;
				memset( &oFDesc, 0, sizeof( oFDesc ) );

				char sFilename[ _MAX_FNAME ];
				char sExt[ _MAX_EXT ];
				_splitpath( pPath, NULL, NULL, sFilename, sExt );
				sprintf( oFDesc.cFileName, "%s%s", sFilename, sExt );

				pDir->AddEntry( &oFDesc, NULL, NULL, pFile );

			}
		}

		znext( pFile );
	}


	return pRoot;
}


static unsigned short SWAP16(unsigned short d)
{
	return ((d>>8) | (d<<8));
}


static	int		ComputeRLE(unsigned char *p,unsigned char data,int todo)
{
	int nb = 0;
	while ((todo > 0) && (*p == data))
	{
		nb++;
		todo--;
		p++;
	}
	return nb;
}

bool	CFloppy::WriteImage(const char *pName)
{
	FILE *h = fopen(pName,"wb");
	if (h)
	{

		FAT_Flush();

		MSAHEADER header;
		header.ID = 0x0f0e;
		header.StartTrack = 0;
		header.EndTrack = SWAP16(m_nbCylinder-1);
		header.Sectors = SWAP16(m_nbSectorPerTrack);
		header.Sides = SWAP16(m_nbSide-1);
		fwrite(&header,1,sizeof(header),h);

		unsigned char *pTempBuffer = new unsigned char [32*1024];

		int rawSize = m_nbSectorPerTrack * 512;

		for (int t=0;t<m_nbCylinder * m_nbSide;t++)
		{
			unsigned char *pR = (unsigned char*)m_pRawImage + t * rawSize;
			unsigned char *pTemp = pTempBuffer;
			int todo = rawSize;
			
			while (todo > 0)
			{
				unsigned char data = *pR;
				int nRepeat = ComputeRLE(pR,data,todo);
				if ((nRepeat > 4) || (0xe5 == data))
				{	// RLE efficient (or 0xe5 special case)
					*pTemp++ = 0xe5;
					*pTemp++ = data;
					*pTemp++ = (nRepeat>>8)&255;
					*pTemp++ = (nRepeat&255);
					todo -= nRepeat;
					pR += nRepeat;
				}
				else
				{
					*pTemp++ = data;
					todo--;
					pR++;
				}
			}
			
			// check if packing track was efficient
			int packedSize = ((int)pTemp - (int)pTempBuffer);
			if (packedSize < rawSize)
			{
				fputc(packedSize>>8,h);
				fputc(packedSize&255,h);
				fwrite(pTempBuffer,1,packedSize,h);
			}
			else
			{
				fputc(rawSize>>8,h);
				fputc(rawSize&255,h);
				fwrite((unsigned char*)m_pRawImage + t * rawSize,1,rawSize,h);
			}
			
		}

		delete [] pTempBuffer;

		return true;
	}
	return false;
}





static	void	LFNStrCpy(char *pDst,const char *pSrc,int len)
{
	for (int i=0;i<len;i++)
	{
		if ((*pSrc) && (*pSrc != '.'))
		{
			*pDst++ = *pSrc++;
		}
		else
			*pDst++ = 0x20;
	}
}

void	CDirEntry::LFN_Create(LFN *pLFN,int clusterStart)
{
	memset(pLFN,0,sizeof(LFN));
	char sFname[_MAX_FNAME];
	char sExt[_MAX_FNAME];

	_splitpath(GetName(),NULL,NULL,sFname,sExt);
	strupr(sFname);
	strupr(sExt);

	char *pExt = sExt;
	if ('.' == *pExt)
		pExt++;

	LFNStrCpy(pLFN->sName,sFname,8);
	LFNStrCpy(pLFN->sExt,pExt,3);

	if (IsDirectory())
		pLFN->attrib = (1<<4);		// directory

	pLFN->firstCluster = clusterStart;

	if (!IsDirectory())
		pLFN->fileSize = GetSize();

	FileTimeToDosDateTime(&m_info.ftLastWriteTime,&pLFN->updateDate,&pLFN->updateTime);

}

unsigned char	*	CFloppy::GetRawAd(int cluster)
{
	return m_pRawImage + 512 * (1+SECTOR_PER_FAT*2+ROOTDIR_NBSECTOR) + 1024*(cluster-2);		// always two reserved clusters
}

bool	CFloppy::BuildDirectory(LFN *pLFN,CDirectory *pDir,int cluster,int parentCluster,int size,int level)
{

	// clear the directory file
	memset(pLFN,0,size);

	if (cluster > 0)
	{
		// Create "." directory
		memset(pLFN,0,sizeof(LFN));
		pLFN->attrib = 0x10;			// directory
		memset(pLFN->sName,0x20,8+3);
		strcpy(pLFN->sName,".");
		pLFN->firstCluster = cluster;
		pLFN++;

		// Create ".." directory
		memset(pLFN,0,sizeof(LFN));
		pLFN->attrib = 0x10;			// directory
		memset(pLFN->sName,0x20,8+3);
		strcpy(pLFN->sName,"..");
		pLFN->firstCluster = parentCluster;
		pLFN++;

	}
	else
	{	// Root first entry, create volume name
		memset(pLFN,0,sizeof(LFN));
		LFNStrCpy(pLFN->sName,"LEONARD",8);
		LFNStrCpy(pLFN->sExt,"",3);
		pLFN->attrib = 0x8;
		pLFN++;
	}

	CDirEntry *pEntry = pDir->GetFirstEntry();
	while (pEntry)
	{
		for (int i=0;i<level;i++)
			printf("  ");

		CDirectory *pSubDir = pEntry->GetDirectory();			
		if (pSubDir)
		{
			printf("[%s]\n",pEntry->GetName());
			// reserve space for directory
			int nbCluster = (((pSubDir->GetNbEntry()+2)*32)+1023)/1024;		// nbentry+2 because of "." and ".." directory

			if (nbCluster > m_nbFreeCluster)
			{
				printf("ERROR: No more space on the disk.\n");
				return false;
			}

			int SubDirCluster = m_nextCluster;

			pEntry->LFN_Create(pLFN,SubDirCluster);

			for (int i=0;i<nbCluster-1;i++)
				m_pFat[SubDirCluster+i] = SubDirCluster+i+1;

			m_pFat[SubDirCluster+nbCluster-1] = -1;		// end chain marker

			m_nextCluster += nbCluster;
			m_nbFreeCluster -= nbCluster;

			if (!BuildDirectory((LFN*)GetRawAd(SubDirCluster),pSubDir,SubDirCluster,cluster,nbCluster*1024,level+1))
				return false;

		}
		else
		{
			printf("%s\n",pEntry->GetName());
			// create file entry
			int nbCluster = (pEntry->GetSize()+1023)/1024;

			if (nbCluster > 0)
			{
				if (nbCluster > m_nbFreeCluster)
				{
					printf("ERROR: No more space on the disk.\n");
					return false;
				}

				int fileCluster = m_nextCluster;
				pEntry->LFN_Create(pLFN,fileCluster);

				if ( pEntry->GetSize() >= 0 )
				{
					memcpy( GetRawAd(fileCluster), pEntry->m_pFileData, pEntry->GetSize() );
				}
				else
				{
					printf("ERROR: Could not load host file \"%s\"",pEntry->GetHostName());
					return false;
				}

				for (int i=0;i<nbCluster-1;i++)
					m_pFat[fileCluster+i] = fileCluster+i+1;

				m_pFat[fileCluster+nbCluster-1] = -1;		// end chain marker
			}
			else
			{	// special case for 0 bytes files !!
				pEntry->LFN_Create(pLFN,0);				// 0 byte file use "0" as first cluster
			}
			m_nextCluster += nbCluster;
			m_nbFreeCluster -= nbCluster;

		}

		pLFN++;
		pEntry = pEntry->GetNext();
	}
	return true;
}


void	CFloppy::FAT_Flush()
{


	unsigned char *pFat = m_pRawImage + 512;
	memset(pFat,0,SECTOR_PER_FAT*512);

	pFat[0] = 0xf7;
	pFat[1] = 0xff;
	pFat[2] = 0xff;

	unsigned char *p = pFat + 3;

	for (int i=2;i<m_maxFatEntry;i+=2)
	{

		unsigned int a = m_pFat[i]&0xfff;
		unsigned int b = 0;
		
		if ((i+1)<m_maxFatEntry)
			b = m_pFat[i+1]&0xfff;

		p[0] = a&0xff;
		p[1] = (a>>8) | ((b&0xf)<<4);
		p[2] = (b>>4);
		p += 3;
	}

	// duplicate second fat
	memcpy(pFat + SECTOR_PER_FAT*512,pFat,SECTOR_PER_FAT*512);

}

bool	CFloppy::Fill(CDirectory *pRoot)
{

	if ((pRoot->GetNbEntry()+1) > MAX_ROOT_ENTRY)
	{
		printf("ERROR: Too much files in root directory (%d > %d)\n",pRoot->GetNbEntry(),MAX_ROOT_ENTRY);
		return false;
	}

	LFN *pLFN = (LFN*)(m_pRawImage + 512 * (1+2*SECTOR_PER_FAT));

	// Root dir is special: there is a reserved space after boot and fats
	if (BuildDirectory(pLFN,pRoot,0,0,ROOTDIR_NBSECTOR * 512,0))
	{
		printf("Free data cluster: %d\n",m_nbFreeCluster);
		return true;
	}

	return false;
}



void	ZIPParse()
{
	ZFILE*	pFile = zopen( "test.zip", "rb" );
	if ( pFile )
	{

		for (;;)
		{

			char* sName = zname( pFile );
			if ( !sName )
				break;

			printf( "%s\n", sName );

			znext( pFile );
		}


		zclose( pFile );
	}
}



int main(int argc, char* argv[])
{

	printf(	"Dir2Msa v1.1 (beta)\n"
			"Make an ATARI MSA floppy disk image from\n"
			"ZIP file archive or a windows directory.\n"
			"Written by Leonard/OXYGENE\n\n");

	if (32 != sizeof(LFN)) return -1;		// Change the LFN struct depending on your compiler settings (should be 32bytes long)

	int rCode = -1;


/*
	ZIPParse();
	return 0;
*/


	if (argc != 2)
	{
		printf(	"Usage: dir2msa <directory path>\n"
				"ex: dir2floppy c:\\harddisk\\demo1\n"
				"    copy every files and folders from c:\\harddisk\\demo1\\*.* to\n"
				"    c:\\harddisk\\demo1.msa file.\n");
	}
	else
	{

		FileDescriptor info;
		if (INVALID_HANDLE_VALUE != FindFirstFile(argv[1],&info))
		{

			CDirectory *pDir = NULL;
			char sImageName[_MAX_PATH];

			if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			{
				printf("Parsing directory tree...\n");
				sprintf(sImageName,"%s.msa",argv[1]);
				pDir = CreateTreeFromDirectory(argv[1]);
			}
			else
			{	// maybe it's a ZIP file
				ZFILE* pZIP = zopen( argv[1], "rb" );
				if ( pZIP )
				{
					printf("Parsing ZIP archive file...\n");

					char sDrive[ _MAX_DRIVE ];
					char sDirName[ _MAX_DIR ];
					char sFname[ _MAX_FNAME ];
					_splitpath( argv[1], sDrive, sDirName, sFname, NULL );
					_makepath( sImageName, sDrive, sDirName, sFname, ".msa" );

					pDir = CreateTreeFromZIP( argv[ 1 ], pZIP );
				//	zclose( pZIP );
				}
			}

			if (pDir)
			{
				CFloppy floppy;
				floppy.Create(NB_HEAD,NB_SECTOR_PER_TRACK,NB_CYLINDER);

				bool bOk = floppy.Fill( pDir );
				if (!bOk)
				{
					printf("Try to generate a 11 sector floppy...\n");
					floppy.Destroy();
					floppy.Create(NB_HEAD,NB_SECTOR_PER_TRACK+1,NB_CYLINDER);
					bOk = floppy.Fill( pDir );
				}

				if (bOk)
				{
					printf("\nWriting file \"%s\"\n",sImageName);
					floppy.WriteImage(sImageName);
					rCode = 0;		// return with no errors
				}

				delete pDir;
			}
			else
			{
				printf("ERROR on \"%s\":\nNot a directory, or not a ZIP file\n",argv[1]);
			}
		}
		else
		{
			printf("ERROR: \"%s\" is not a valid path\n",argv[1]);
		}
	}	

	return rCode;
}

