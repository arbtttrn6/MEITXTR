#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

typedef unsigned char  BYTE;
typedef unsigned short WORD;
typedef unsigned int   DWORD;


static DWORD ReadDWORD(const BYTE* p)
{
	return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}


static void WriteDWORD(BYTE* p, DWORD v)
{
	p[0] = (BYTE)( v        & 0xFF);
	p[1] = (BYTE)((v >>  8) & 0xFF);
	p[2] = (BYTE)((v >> 16) & 0xFF);
	p[3] = (BYTE)((v >> 24) & 0xFF);
}


static DWORD Fnv1(const BYTE* pData, DWORD dwLen)
{
	DWORD h = 0x811C9DC5u;
	for (DWORD i = 0; i < dwLen; ++i)
	{
		h *= 0x01000193u;
		h ^= pData[i];
	}
	return h;
}


struct SBlock
{
	DWORD dwSignature;
	DWORD dwOffset;
	DWORD dwLength;
	DWORD dwChecksum;
	DWORD dwDataOffset;
};


struct SStringEntry
{
	DWORD dwOrigOffset;
	DWORD dwOrigLength;
	char* pszText;
	bool  bChanged;
};


class CStringEditor
{
public:
	CStringEditor();
	~CStringEditor();

	bool Load(const char* pszFileName);
	bool Save(const char* pszFileName);

	int  GetStringCount() const { return (int)m_vStrings.size(); }
	const char* GetString(int nIndex) const;
	bool SetString(int nIndex, const char* pszNewText);
	void PrintAll() const;

private:
	BYTE*                     m_pFileData;
	DWORD                     m_dwFileSize;
	std::vector<SStringEntry> m_vStrings;
	std::vector<SBlock>       m_vBlocks;

	void Clear();
	bool ReadWholeFile(const char* pszFileName);
	bool ParseBlocks();
	const SBlock* FindBlock(DWORD dwSignature) const;
	bool ExtractStrings();
	char* ReadStringAt(DWORD dwAbsOffset, DWORD dwLen) const;

	bool RebuildAndSave(const char* pszFileName);
	bool BuildNewCdat(std::vector<BYTE>& vOut) const;
	bool BuildNewChdr(std::vector<BYTE>& vOut) const;
};


CStringEditor::CStringEditor()
	: m_pFileData(NULL), m_dwFileSize(0)
{
}


CStringEditor::~CStringEditor()
{
	Clear();
}


void CStringEditor::Clear()
{
	if (m_pFileData)
	{
		free(m_pFileData); m_pFileData = NULL;
	}
	m_dwFileSize = 0;
	for (size_t i = 0; i < m_vStrings.size(); ++i)
	{
		if (m_vStrings[i].pszText)
		{
			free(m_vStrings[i].pszText);
		}
	}
	m_vStrings.clear();
	m_vBlocks.clear();
}


bool CStringEditor::ReadWholeFile(const char* pszFileName)
{
	FILE* pFile = fopen(pszFileName, "rb");

	if (!pFile)
	{
		return false;
	}

	fseek(pFile, 0, SEEK_END);
	long lSize = ftell(pFile);
	fseek(pFile, 0, SEEK_SET);

	if (lSize <= 0)
	{
		fclose(pFile);
		return false;
	}

	m_pFileData = (BYTE*)malloc((size_t)lSize);

	if (!m_pFileData)
	{
		fclose(pFile);
		return false;
	}

	size_t nRead = fread(m_pFileData, 1, (size_t)lSize, pFile);
	fclose(pFile);

	if (nRead != (size_t)lSize)
	{
		free(m_pFileData);
		m_pFileData = NULL;
		return false;
	}

	m_dwFileSize = (DWORD)lSize;

	return true;
}


bool CStringEditor::Load(const char* pszFileName)
{
	Clear();
	if (!ReadWholeFile(pszFileName))
	{
		fprintf(stderr, "Fajled tu read ze fajl: %s\n", pszFileName);
		return false;
	}
	if (m_dwFileSize < 8 || ReadDWORD(m_pFileData) != 0x4E494253)
	{
		fprintf(stderr, "Ze fajl does not start with SBIN.\n");
		return false;
	}

	if (!ParseBlocks())
	{
		fprintf(stderr, "Fajled tu pars bloks\n");
		return false;
	}
	if (!ExtractStrings())
	{
		fprintf(stderr, "Fajled tu ekstrakt rows\n");
		return false;
	}

	return true;
}


bool CStringEditor::ParseBlocks()
{
	DWORD dwPos = 8;
	while (dwPos + 12 <= m_dwFileSize)
	{
		DWORD dwSig = ReadDWORD(m_pFileData + dwPos);
		DWORD dwLen = ReadDWORD(m_pFileData + dwPos + 4);

		if (dwSig == 0 || dwSig == 0xFFFFFFFF)
		{
			break;
		}
		if (dwPos + 12 + dwLen > m_dwFileSize)
		{
			break;
		}

		SBlock blk;
		blk.dwSignature  = dwSig;
		blk.dwOffset     = dwPos;
		blk.dwLength     = dwLen;
		blk.dwChecksum   = ReadDWORD(m_pFileData + dwPos + 8);
		blk.dwDataOffset = dwPos + 12;

		m_vBlocks.push_back(blk);
		dwPos += 12 + dwLen;
	}

	return !m_vBlocks.empty();
}


const SBlock* CStringEditor::FindBlock(DWORD dwSignature) const
{
	for (size_t i = 0; i < m_vBlocks.size(); ++i)
	{
		if (m_vBlocks[i].dwSignature == dwSignature)
		{
			return &m_vBlocks[i];
		}
	}

	return NULL;
}


char* CStringEditor::ReadStringAt(DWORD dwAbsOffset, DWORD dwLen) const
{
	if (dwAbsOffset + dwLen > m_dwFileSize)
	{
		return NULL;
	}
	char* psz = (char*)malloc(dwLen + 1);
	if (!psz)
	{
		return NULL;
	}
	memcpy(psz, m_pFileData + dwAbsOffset, dwLen);
	psz[dwLen] = '\0';

	return psz;
}


bool CStringEditor::ExtractStrings()
{
	const SBlock* pCdat = FindBlock(0x54414443); // CDAT
	const SBlock* pChdr = FindBlock(0x52444843); // CHDR

	if (!pCdat || !pChdr)
	{
		return false;
	}

	DWORD dwCdatBase = pCdat->dwDataOffset;
	DWORD dwChdrBase = pChdr->dwDataOffset;
	DWORD dwChdrSize = pChdr->dwLength;

	DWORD dwEntryCount = dwChdrSize / 8;
	for (DWORD i = 0; i < dwEntryCount; ++i)
	{
		DWORD dwOff = ReadDWORD(m_pFileData + dwChdrBase + i * 8);
		DWORD dwLen = ReadDWORD(m_pFileData + dwChdrBase + i * 8 + 4);

		char* psz;
		if (dwLen == 0)
		{
			psz = (char*)malloc(1);
			if (psz) psz[0] = '\0';
		}
		else
		{
			DWORD dwAbs = dwCdatBase + dwOff;
			if (dwAbs + dwLen > m_dwFileSize)
			{
				continue;
			}
			psz = ReadStringAt(dwAbs, dwLen);
		}
		if (!psz)
		{
			continue;
		}

		SStringEntry e;
		e.dwOrigOffset = (dwLen > 0) ? (dwCdatBase + dwOff) : 0;
		e.dwOrigLength = dwLen;
		e.pszText      = psz;
		e.bChanged     = false;

		m_vStrings.push_back(e);
	}

	return !m_vStrings.empty();
}


const char* CStringEditor::GetString(int nIndex) const
{
	if (nIndex < 0 || nIndex >= (int)m_vStrings.size())
	{
		return NULL;
	}

	return m_vStrings[nIndex].pszText;
}


bool CStringEditor::SetString(int nIndex, const char* pszNewText)
{
	if (nIndex < 0 || nIndex >= (int)m_vStrings.size())
	{
		return false;
	}
	if (!pszNewText)
	{
		return false;
	}

	SStringEntry& e = m_vStrings[nIndex];
	size_t nNewLen = strlen(pszNewText);

	char* pszCopy = (char*)malloc(nNewLen + 1);
	if (!pszCopy)
	{
		return false;
	}
	memcpy(pszCopy, pszNewText, nNewLen + 1);

	free(e.pszText);
	e.pszText  = pszCopy;
	e.bChanged = true;

	return true;
}


bool CStringEditor::BuildNewCdat(std::vector<BYTE>& vOut) const
{
	vOut.clear();
	for (size_t i = 0; i < m_vStrings.size(); ++i)
	{
		const char* psz = m_vStrings[i].pszText;
		if (!psz)
		{
			psz = "";
		}
		size_t nLen = strlen(psz);

		vOut.insert(vOut.end(), (const BYTE*)psz, (const BYTE*)psz + nLen);
		vOut.push_back(0);
	}

	return true;
}


bool CStringEditor::BuildNewChdr(std::vector<BYTE>& vOut) const
{
	vOut.clear();
	DWORD dwOff = 0;
	for (size_t i = 0; i < m_vStrings.size(); ++i)
	{
		const char* psz = m_vStrings[i].pszText;
		if (!psz)
		{
			psz = "";
		}
		DWORD dwLen = (DWORD)strlen(psz);

		BYTE aOff[4], aLen[4];
		WriteDWORD(aOff, dwOff);
		WriteDWORD(aLen, dwLen);
		vOut.insert(vOut.end(), aOff, aOff + 4);
		vOut.insert(vOut.end(), aLen, aLen + 4);

		dwOff += dwLen + 1;
	}

	return true;
}


bool CStringEditor::RebuildAndSave(const char* pszFileName)
{
	const SBlock* pCdat = FindBlock(0x54414443); // CDAT
	const SBlock* pChdr = FindBlock(0x52444843); // CHDR

	if (!pCdat || !pChdr)
	{
		return false;
	}

	std::vector<BYTE> vNewCdat;
	std::vector<BYTE> vNewChdr;

	if (!BuildNewCdat(vNewCdat))
	{
		return false;
	}
	if (!BuildNewChdr(vNewChdr))
	{
		return false;
	}

	DWORD dwNewCdatChecksum = 0;
	DWORD dwNewChdrChecksum = 0;
	if (!vNewCdat.empty())
	{
		dwNewCdatChecksum = Fnv1(&vNewCdat[0], (DWORD)vNewCdat.size());
	}
	if (!vNewChdr.empty())
	{
		dwNewChdrChecksum = Fnv1(&vNewChdr[0], (DWORD)vNewChdr.size());
	}

	std::vector<BYTE> vOut;
	vOut.insert(vOut.end(), m_pFileData, m_pFileData + 8);

	for (size_t i = 0; i < m_vBlocks.size(); ++i)
	{
		const SBlock& b = m_vBlocks[i];

		if (b.dwSignature == 0x52444843) // CHDR
		{
			BYTE aHdr[12];
			WriteDWORD(aHdr + 0, b.dwSignature);
			WriteDWORD(aHdr + 4, (DWORD)vNewChdr.size());
			WriteDWORD(aHdr + 8, dwNewChdrChecksum);
			vOut.insert(vOut.end(), aHdr, aHdr + 12);
			vOut.insert(vOut.end(), vNewChdr.begin(), vNewChdr.end());
		}
		else if (b.dwSignature == 0x54414443) // CDAT
		{
			BYTE aHdr[12];
			WriteDWORD(aHdr + 0, b.dwSignature);
			WriteDWORD(aHdr + 4, (DWORD)vNewCdat.size());
			WriteDWORD(aHdr + 8, dwNewCdatChecksum);
			vOut.insert(vOut.end(), aHdr, aHdr + 12);
			vOut.insert(vOut.end(), vNewCdat.begin(), vNewCdat.end());
		}
		else
		{
			const BYTE* pSrc = m_pFileData + b.dwOffset;
			vOut.insert(vOut.end(), pSrc, pSrc + 12 + b.dwLength);
		}
	}

	{
		const SBlock& bLast = m_vBlocks[m_vBlocks.size() - 1];
		DWORD dwLastEnd = bLast.dwOffset + 12 + bLast.dwLength;
		if (dwLastEnd < m_dwFileSize)
		{
			vOut.insert(vOut.end(),
						m_pFileData + dwLastEnd,
						m_pFileData + m_dwFileSize);
		}
	}

	FILE* pFile = fopen(pszFileName, "wb");

	if (!pFile)
	{
		return false;
	}

	size_t nWritten = fwrite(&vOut[0], 1, vOut.size(), pFile);
	fclose(pFile);

	return nWritten == vOut.size();
}


bool CStringEditor::Save(const char* pszFileName)
{
	return RebuildAndSave(pszFileName);
}


void CStringEditor::PrintAll() const
{
	printf("Total lajns: %d\n", (int)m_vStrings.size());
	for (size_t i = 0; i < m_vStrings.size(); ++i)
	{
		const SStringEntry& e = m_vStrings[i];
		printf("[%3u] off=0x%08X len=%u%s : \"%s\"\n",
			   (unsigned)i,
			   (unsigned)e.dwOrigOffset,
			   (unsigned)e.dwOrigLength,
			   e.bChanged ? " (czanzsed)" : "",
			   e.pszText ? e.pszText : "");
	}
}


const char* get_program_version() {
	static char szFullVersion[17] = "1.0.0.00000000";

	if (szFullVersion[6] == '0' && szFullVersion[7] == '0')
	{
		const char* pszD = __DATE__;

		szFullVersion[6] = (pszD[4] == ' ') ? '0' : pszD[4];
		szFullVersion[7] = pszD[5];

		const char* pszM = "JanFebMarAprMayJunJulAugSepOctNovDec";
		int nMonth = 1;
		for (int nI = 0; nI < 12; ++nI)
		{
			if (pszD[0] == pszM[nI * 3] && pszD[1] == pszM[nI * 3 + 1])
			{
				nMonth = nI + 1;
				break;
			}
		}
		szFullVersion[8] = (char)('0' + nMonth / 10);
		szFullVersion[9] = (char)('0' + nMonth % 10);

		szFullVersion[10] = pszD[7];
		szFullVersion[11] = pszD[8];
		szFullVersion[12] = pszD[9];
		szFullVersion[13] = pszD[10];
	}

	return szFullVersion;
}

void PrintInfo(const char* pszProgramName)
{
	const char* const pszProgramVersion = get_program_version();
	printf("author: Artur Ajvazjan (arbtttrn6)\n");
	printf("version: %s\n", pszProgramVersion);
	printf("usage:\n");
	printf("  %s <input.bin> --list                                - View all strings\n",
		   pszProgramName);
	printf("  %s <input.bin> --dump <output.txt>                   - Dump all strings in tekst file\n",
		   pszProgramName);
	printf("  %s <input.bin> --set <index> <new_text> <output.bin> - Change string by indeks\n",
		   pszProgramName);
	printf("  %s <input.bin> --save <output.bin>                   - Resave file\n",
		   pszProgramName);
}


int main(int argc, char* argv[])
{
	if (argc < 3)
	{
		PrintInfo(argv[0]);
		return 1;
	}

	CStringEditor editor;

	if (!editor.Load(argv[1]))
	{
		return 2;
	}

	const char* pszCmd = argv[2];

	if (strcmp(pszCmd, "--list") == 0)
	{
		editor.PrintAll();
	}
	else if (strcmp(pszCmd, "--dump") == 0 && argc >= 4)
	{
		FILE* pOut = fopen(argv[3], "w");

		if (!pOut)
		{
			fprintf(stderr, "Cannot open %s\n", argv[3]);
			return 3;
		}

		for (int i = 0; i < editor.GetStringCount(); ++i)
		{
			const char* psz = editor.GetString(i);
			fprintf(pOut, "%d\t%s\n", i, psz ? psz : "");
		}

		fclose(pOut);
	}
	else if (strcmp(pszCmd, "--set") == 0 && argc >= 6)
	{
		int nIndex = atoi(argv[3]);
		const char* pszNew = argv[4];
		const char* pszOut = argv[5];

		if (!editor.SetString(nIndex, pszNew))
		{
			fprintf(stderr, "Fajled tu zet ze string %d\n", nIndex);
			return 4;
		}
		if (!editor.Save(pszOut))
		{
			fprintf(stderr, "Fajled tu sejv %s\n", pszOut);
			return 5;
		}
		printf("Sejved in %s\n", pszOut);
	}
	else if (strcmp(pszCmd, "--save") == 0 && argc >= 4)
	{
		if (!editor.Save(argv[3]))
		{
			fprintf(stderr, "Fajled tu sejv %s\n", argv[3]);
			return 5;
		}
		printf("Sejved in %s\n", argv[3]);
	}
	else
	{
		fprintf(stderr, "Anknown kommand: %s\n", pszCmd);
		return 6;
	}
	return 0;
}
