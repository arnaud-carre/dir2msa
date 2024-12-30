
#ifndef __DIR2FLOPPY__
#define __DIR2FLOPPY__

#include "zip/zipio.h"

typedef		WIN32_FIND_DATA		FileDescriptor;

class CDirectory;

struct	LFN
{
	char			sName[8];
	char			sExt[3];
	unsigned char	attrib;
	unsigned char	pad[10];		// dummy pad (not used on FAT12)
	unsigned short	updateTime;
	unsigned short	updateDate;
	unsigned short	firstCluster;
	unsigned long	fileSize;
};

struct MSAHEADER
{
	unsigned short	ID,Sectors, Sides, StartTrack, EndTrack;
};

class CDirEntry
{
public:
	CDirEntry();
	~CDirEntry();


	void				Create(const FileDescriptor *pInfo,CDirectory *pSubDir,const char *pHostName, ZFILE* pZIP = NULL );

	CDirectory		*	GetDirectory()		{ return m_pDirectory; }
	const char		*	GetName() const
	{
		if (*m_info.cAlternateFileName)
			return m_info.cAlternateFileName;
		else
			return m_info.cFileName;
	}

	const char		*	GetHostName() const	{ return m_sHostName; }
	int					GetSize()			{ return m_info.nFileSizeLow; }

	bool				IsDirectory() const	{ return NULL != m_pDirectory; }
	CDirEntry		*	GetNext()		{ return m_pNext; }
	void				SetNext(CDirEntry *pNext)		{ m_pNext = pNext; }

	void				LFN_Create(LFN *pLFN,int clusterStart);

public:
	FileDescriptor		m_info;
	char				m_sHostName[_MAX_PATH];

	void*				m_pFileData;

	CDirectory		*	m_pDirectory;
	CDirEntry		*	m_pNext;
};

class CDirectory
{
public:
	CDirectory();
	~CDirectory();

	void	AddEntry(const FileDescriptor *pInfo,CDirectory *pSubDir,const char *pHostName, ZFILE* pZIP = NULL );

	void	Dump(const char *pPath);
	int		GetNbEntry() const				{ return m_nbEntry; }
	CDirEntry	*	GetFirstEntry()	const	{ return m_pEntryList; }

	CDirectory*		DirExist()	const;

private:
	int				m_nbEntry;
	CDirEntry	*	m_pEntryList;
};


class CFloppy
{
public:
	CFloppy();
	~CFloppy();

	bool			Create(int nbSide,int nbSectorPerTrack,int nbCylinder);
	void			Destroy();

	bool			Fill(CDirectory *pRoot);
	bool			WriteImage(const char *pName);

private:

	void			FAT_Flush();
	bool			BuildDirectory(LFN *pLFN,CDirectory *pDir,int cluster,int parentCluster,int size,int level);
	unsigned char *	GetRawAd(int cluster);
	void			w8(int offset,unsigned char d)		{ m_pRawImage[offset] = d; }
	void			w16(int offset,unsigned short d)	{ m_pRawImage[offset] = d&0xff; m_pRawImage[offset+1] = (d>>8); }

	int					m_nbSide;
	int					m_nbCylinder;
	int					m_nbSectorPerTrack;
	int					m_rawSize;

	CDirectory		*	m_pRoot;
	unsigned char	*	m_pRawImage;

	int					m_nbFreeCluster;
	int					m_nextCluster;
	int					m_maxFatEntry;
	int					m_nbFatEntry;
	int				*	m_pFat;


};



#endif // __DIR2FLOPPY__