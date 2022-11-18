#include <cassert>
#include <algorithm>
#include "string_format.h"
#include "../states/RegisterStateFile.h"
#include "../FrameDump.h"
#include "GIF.h"
#include "Dmac_Channel.h"
#include "Vpu.h"
#include "Vif1.h"
#include "ThreadUtils.h"

#define STATE_PATH_FORMAT ("vpu/vif1_%d.xml")
#define STATE_REGS_BASE ("BASE")
#define STATE_REGS_TOP ("TOP")
#define STATE_REGS_TOPS ("TOPS")
#define STATE_REGS_OFST ("OFST")
#define STATE_REGS_DIRECTQWORDBUFFER ("directQwordBuffer")
#define STATE_REGS_DIRECTQWORDBUFFER_INDEX ("directQwordBufferIndex")

CVif1::CVif1(unsigned int number, CVpu& vpu, CGIF& gif, CINTC& intc, uint8* ram, uint8* spr)
    : CVif(1, vpu, intc, ram, spr)
    , m_gif(gif)
{
	m_dmaBuffer.resize(g_dmaBufferSize);
	m_vifThread = std::thread([this]() { ThreadProc(); });
	Framework::ThreadUtils::SetThreadName(m_vifThread, "VIF1 Thread");
}

CVif1::~CVif1()
{
	m_vifThread.detach();
}

void CVif1::ThreadProc()
{
	uint32 readQwAmount = 0;
	while(1)
	{
		uint32 qwAmount = 0;
		{
			std::unique_lock readLock{m_ringBufferMutex};
			m_dmaBufferReadPos += readQwAmount;
			m_dmaBufferReadPos %= g_dmaBufferSize;
			m_dmaBufferContentsSize -= readQwAmount;
			readQwAmount = 0;
			if(m_dmaBufferContentsSize == 0)
			{
				m_processing = false;
			}
			m_consumedDataCondVar.notify_one();
			while(1)
			{
				if(m_dmaBufferPaused)
				{
					m_hasDataCondVar.wait(readLock, [this]() { return m_dmaBufferResumeRq; });
					assert(m_dmaBufferResumeRq);
					m_dmaBufferPaused = false;
					m_dmaBufferResumeRq = false;
					m_pauseAckCondVariable.notify_one();
				}
				else
				{
					if(m_dmaBufferPauseRq)
					{
						m_dmaBufferPaused = true;
						m_dmaBufferPauseRq = false;
						m_pauseAckCondVariable.notify_one();
						continue;
					}
					if(m_dmaBufferContentsSize > 0) break;
					m_hasDataCondVar.wait(readLock, [this]() { return (m_dmaBufferContentsSize != 0) || m_dmaBufferPauseRq; });
				}
			}
			qwAmount = std::min<uint32>(g_dmaBufferSize - m_dmaBufferReadPos, m_dmaBufferContentsSize);
		}
		uint32 transferSize = qwAmount * 0x10;
		m_stream.SetFifoParams(reinterpret_cast<uint8*>(m_dmaBuffer.data() + m_dmaBufferReadPos), transferSize);
		ProcessPacket(m_stream);
		uint32 newIndex = m_stream.GetRemainingDmaTransferSize();
		uint32 discardSize = transferSize - newIndex;
		assert((discardSize & 0x0F) == 0);
		readQwAmount = discardSize / 0x10;
	}
}

void CVif1::Reset()
{
	assert(m_dmaBufferPaused);
	CVif::Reset();
	m_BASE = 0;
	m_TOP = 0;
	m_TOPS = 0;
	m_OFST = 0;
	m_directQwordBufferIndex = 0;
	memset(&m_directQwordBuffer, 0, sizeof(m_directQwordBuffer));
	m_dmaBufferContentsSize = 0;
	m_dmaBufferReadPos = 0;
	m_dmaBufferWritePos = 0;
	m_processing = false;
}

void CVif1::SaveState(Framework::CZipArchiveWriter& archive)
{
	assert(m_dmaBufferPaused);
	CVif::SaveState(archive);

	auto path = string_format(STATE_PATH_FORMAT, m_number);
	CRegisterStateFile* registerFile = new CRegisterStateFile(path.c_str());
	registerFile->SetRegister32(STATE_REGS_BASE, m_BASE);
	registerFile->SetRegister32(STATE_REGS_TOP, m_TOP);
	registerFile->SetRegister32(STATE_REGS_TOPS, m_TOPS);
	registerFile->SetRegister32(STATE_REGS_OFST, m_OFST);
	registerFile->SetRegister128(STATE_REGS_DIRECTQWORDBUFFER, *reinterpret_cast<uint128*>(&m_directQwordBuffer));
	registerFile->SetRegister32(STATE_REGS_DIRECTQWORDBUFFER_INDEX, m_directQwordBufferIndex);
	archive.InsertFile(registerFile);
}

void CVif1::LoadState(Framework::CZipArchiveReader& archive)
{
	assert(m_dmaBufferPaused);
	CVif::LoadState(archive);

	auto path = string_format(STATE_PATH_FORMAT, m_number);
	CRegisterStateFile registerFile(*archive.BeginReadFile(path.c_str()));
	m_BASE = registerFile.GetRegister32(STATE_REGS_BASE);
	m_TOP = registerFile.GetRegister32(STATE_REGS_TOP);
	m_TOPS = registerFile.GetRegister32(STATE_REGS_TOPS);
	m_OFST = registerFile.GetRegister32(STATE_REGS_OFST);
	*reinterpret_cast<uint128*>(&m_directQwordBuffer) = registerFile.GetRegister128(STATE_REGS_DIRECTQWORDBUFFER);
	m_directQwordBufferIndex = registerFile.GetRegister32(STATE_REGS_DIRECTQWORDBUFFER_INDEX);

	//TODO: Save this state
	m_dmaBufferContentsSize = 0;
	m_dmaBufferReadPos = 0;
	m_dmaBufferWritePos = 0;
	m_processing = false;
}

uint32 CVif1::GetTOP() const
{
	return m_TOP;
}

void CVif1::FlushPendingXgKicks()
{
	while(m_pendingXgKicksSize != 0)
	{
		if(m_gif.TryAcquirePath(1))
		{
			m_pendingXgKicksSize--;
			auto& pendingXgKick = m_pendingXgKicks[m_pendingXgKicksSize];
			ProcessXgKickGifPacket(pendingXgKick.memory, pendingXgKick.address, pendingXgKick.metadata);
		}
		else
		{
			break;
		}
	}
}

void CVif1::ProcessXgKickGifPacket(const uint8* vuMemory, uint32 address, const CGsPacketMetadata& metadata)
{
	address += m_gif.ProcessSinglePacket(vuMemory, PS2::VUMEM1SIZE, address, PS2::VUMEM1SIZE, metadata);
	if((address == PS2::VUMEM1SIZE) && (m_gif.GetActivePath() == 1))
	{
		address = 0;
		address += m_gif.ProcessSinglePacket(vuMemory, PS2::VUMEM1SIZE, address, PS2::VUMEM1SIZE, metadata);
	}
	assert(m_gif.GetActivePath() == 0);
}

void CVif1::ProcessXgKick(uint32 address)
{
	address &= 0x3FF;
	address *= 0x10;

	CGsPacketMetadata metadata;
	metadata.pathIndex = 1;
#ifdef DEBUGGER_INCLUDED
	metadata.vuMemPacketAddress = address;
	metadata.vpu1Top = GetVuTopMiniState();
	metadata.vpu1Itop = GetVuItopMiniState();
	memcpy(&metadata.vu1State, &GetVuMiniState(), sizeof(MIPSSTATE));
	memcpy(metadata.vuMem1, GetVuMemoryMiniState(), PS2::VUMEM1SIZE);
	memcpy(metadata.microMem1, GetMicroMemoryMiniState(), PS2::MICROMEM1SIZE);
#endif

	if(m_gif.TryAcquirePath(1))
	{
		ProcessXgKickGifPacket(m_vpu.GetVuMemory(), address, metadata);
	}
	else
	{
		assert(m_pendingXgKicksSize != MAX_PENDING_XGKICKS);
		auto& pendingXgKick = m_pendingXgKicks[m_pendingXgKicksSize];
		pendingXgKick.address = address;
		memcpy(pendingXgKick.memory, m_vpu.GetVuMemory(), PS2::VUMEM1SIZE);
		pendingXgKick.metadata = std::move(metadata);
		m_pendingXgKicksSize++;
	}

#ifdef DEBUGGER_INCLUDED
	SaveMiniState();
#endif
}

uint32 CVif1::ReceiveDMA(uint32 address, uint32 qwc, uint32 direction, bool tagIncluded)
{
	uint8* source = nullptr;
	uint32 size = qwc * 0x10;
	if(address & 0x80000000)
	{
		source = m_spr;
		address &= (PS2::EE_SPR_SIZE - 1);
		assert((address + size) <= PS2::EE_SPR_SIZE);
	}
	else
	{
		source = m_ram;
		address &= (PS2::EE_RAM_SIZE - 1);
		assert((address + size) <= PS2::EE_RAM_SIZE);
	}
	if(direction == Dmac::CChannel::CHCR_DIR_TO)
	{
		auto gs = m_gif.GetGsHandler();
		gs->ReadImageData(source + address, size);
		return qwc;
	}
	else
	{
		uint32 qwToWrite = 0;
		{
			std::unique_lock writeLock{m_ringBufferMutex};
			m_consumedDataCondVar.wait(writeLock, [this, qwc]() { return (g_dmaBufferSize - m_dmaBufferContentsSize) >= qwc; });
			qwToWrite = qwc;
		}
		if(tagIncluded)
		{
			assert(qwToWrite == 1);
			uint128 qw = *reinterpret_cast<uint128*>(source + address);
			qw.nD0 = 0;
#ifdef _DEBUG
			for(uint32 i = 2; i < 4; i++)
			{
				auto code = make_convertible<CODE>(qw.nV[i]);
				assert(
				    (code.nCMD == CODE_CMD_NOP) ||
				    (code.nCMD == CODE_CMD_DIRECT) ||
				    (code.nCMD == CODE_CMD_DIRECTHL));
			}
#endif
			m_dmaBuffer[m_dmaBufferWritePos] = qw;
		}
		else
		{
			uint32 firstQwSize = std::min<uint32>(g_dmaBufferSize - m_dmaBufferWritePos, qwToWrite);
			uint32 splitQwSize = qwToWrite - firstQwSize;
			assert(firstQwSize != 0);
			memcpy(m_dmaBuffer.data() + m_dmaBufferWritePos, source + address, firstQwSize * 0x10);
			if(splitQwSize != 0)
			{
				memcpy(m_dmaBuffer.data(), source + address + (firstQwSize * 0x10), splitQwSize * 0x10);
			}
		}
		{
			std::unique_lock writeLock{m_ringBufferMutex};
			m_dmaBufferWritePos += qwToWrite;
			m_dmaBufferWritePos %= g_dmaBufferSize;
			m_dmaBufferContentsSize += qwToWrite;
			if(m_dmaBufferContentsSize != 0)
			{
				m_processing = true;
			}
			m_hasDataCondVar.notify_one();
		}
		return qwToWrite;
	}
}

void CVif1::ProcessFifoWrite(uint32 address, uint32 value)
{
	assert(m_fifoIndex != FIFO_SIZE);
	if(m_fifoIndex == FIFO_SIZE)
	{
		return;
	}
	uint32 index = (address & 0xF) / 4;
	*reinterpret_cast<uint32*>(m_fifoBuffer + m_fifoIndex + index * 4) = value;
	if(index == 3)
	{
		{
			std::unique_lock writeLock{m_ringBufferMutex};
			m_consumedDataCondVar.wait(writeLock, [this]() { return m_dmaBufferContentsSize != g_dmaBufferSize; });
		}

		m_dmaBuffer[m_dmaBufferWritePos] = *reinterpret_cast<uint128*>(m_fifoBuffer);
		uint32 qwToWrite = 1;

		{
			std::unique_lock writeLock{m_ringBufferMutex};
			m_dmaBufferWritePos += qwToWrite;
			m_dmaBufferWritePos %= g_dmaBufferSize;
			m_dmaBufferContentsSize += qwToWrite;
			if(m_dmaBufferContentsSize != 0)
			{
				m_processing = true;
			}
			m_hasDataCondVar.notify_one();
		}
		m_fifoIndex = 0;
	}
}

uint32 CVif1::GetRegister(uint32 address)
{
	uint32 result = CVif::GetRegister(address);
	if(address == VIF1_STAT)
	{
		if(m_processing)
		{
			result |= 0x0F000000;
		}
	}
	return result;
}

void CVif1::SetRegister(uint32 address, uint32 value)
{
	switch(address)
	{
	case VIF1_FBRST:
		if(value & FBRST_RST)
		{
			PauseProcessing();
			assert(m_gif.GetActivePath() != 1);
			assert(m_gif.GetActivePath() != 2);
			m_CODE <<= 0;
			m_STAT <<= 0;
			m_NUM = 0;
			m_dmaBufferContentsSize = 0;
			m_processing = false;
			m_dmaBufferReadPos = 0;
			m_dmaBufferWritePos = 0;
			m_stream.Reset();
			ResumeProcessing();
		}
		if(value & FBRST_FBK || value & FBRST_STP)
		{
			// TODO: We need to properly handle this!
			// But I lack games which leverage it.
			assert(0);
		}
		if(value & FBRST_STC)
		{
			PauseProcessing();
			m_STAT.nVSS = 0;
			m_STAT.nVFS = 0;
			m_STAT.nVIS = 0;
			m_STAT.nINT = 0;
			m_STAT.nER0 = 0;
			m_STAT.nER1 = 0;
			ResumeProcessing();
		}
		break;
	default:
		CVif::SetRegister(address, value);
		break;
	}
}

void CVif1::ResumeProcessing()
{
	std::unique_lock ringBufferLock{m_ringBufferMutex};
	assert(m_dmaBufferPaused && !m_dmaBufferResumeRq && !m_dmaBufferPauseRq);
	m_dmaBufferResumeRq = true;
	m_hasDataCondVar.notify_one();
	m_pauseAckCondVariable.wait(ringBufferLock, [this]() { return !m_dmaBufferPaused; });
	assert(!m_dmaBufferResumeRq);
}

void CVif1::PauseProcessing()
{
	std::unique_lock ringBufferLock{m_ringBufferMutex};
	assert(!m_dmaBufferPaused && !m_dmaBufferResumeRq && !m_dmaBufferPauseRq);
	m_dmaBufferPauseRq = true;
	m_hasDataCondVar.notify_one();
	m_pauseAckCondVariable.wait(ringBufferLock, [this]() { return m_dmaBufferPaused; });
	assert(!m_dmaBufferPauseRq);
}

void CVif1::ExecuteCommand(StreamType& stream, CODE nCommand)
{
#ifdef _DEBUG
	DisassembleCommand(nCommand);
#endif
	if(m_pendingXgKicksSize != 0)
	{
		FlushPendingXgKicks();
		if(m_pendingXgKicksSize != 0)
		{
			m_STAT.nVGW = 1;
			return;
		}
		else
		{
			m_STAT.nVGW = 0;
		}
	}
	switch(nCommand.nCMD)
	{
	case CODE_CMD_OFFSET:
		m_OFST = nCommand.nIMM;
		m_STAT.nDBF = 0;
		m_TOPS = m_BASE;
		break;
	case CODE_CMD_BASE:
		m_BASE = nCommand.nIMM;
		break;
	case CODE_CMD_MSKPATH3:
		m_gif.SetPath3Masked((nCommand.nIMM & 0x8000) != 0);
		break;
	case CODE_CMD_FLUSH:
		if(m_vpu.IsVuRunning())
		{
			m_STAT.nVEW = 1;
		}
		else
		{
			m_STAT.nVEW = 0;
		}
		if(ResumeDelayedMicroProgram())
		{
			m_STAT.nVEW = 1;
			return;
		}
		break;
	case CODE_CMD_FLUSHA:
		if(m_vpu.IsVuRunning())
		{
			m_STAT.nVEW = 1;
		}
		else
		{
			m_STAT.nVEW = 0;
		}
		if(ResumeDelayedMicroProgram())
		{
			m_STAT.nVEW = 1;
			return;
		}
		break;
	case CODE_CMD_DIRECT:
	case CODE_CMD_DIRECTHL:
		Cmd_DIRECT(stream, nCommand);
		break;
	default:
		CVif::ExecuteCommand(stream, nCommand);
		break;
	}
}

void CVif1::Cmd_DIRECT(StreamType& stream, CODE nCommand)
{
	uint32 nSize = stream.GetAvailableReadBytes();
	assert((nSize & 0x03) == 0);

	if(nSize != 0)
	{
		//Check if we have data but less than a qword
		//If we do, we have to go inside a different path to complete a full qword
		bool hasPartialQword = (m_directQwordBufferIndex != 0) || (nSize < QWORD_SIZE);
		if(hasPartialQword)
		{
			//Read enough bytes to try to complete our qword
			assert(m_directQwordBufferIndex < QWORD_SIZE);
			uint32 readAmount = std::min(nSize, QWORD_SIZE - m_directQwordBufferIndex);
			stream.Read(m_directQwordBuffer + m_directQwordBufferIndex, readAmount);
			m_directQwordBufferIndex += readAmount;
			nSize -= readAmount;

			//If our qword is complete, send to GIF
			if(m_directQwordBufferIndex == QWORD_SIZE)
			{
				assert(m_CODE.nIMM != 0);
				uint32 processed = m_gif.ProcessMultiplePackets(m_directQwordBuffer, QWORD_SIZE, 0, QWORD_SIZE, CGsPacketMetadata(2));
				assert(processed == QWORD_SIZE);
				m_CODE.nIMM--;
				m_directQwordBufferIndex = 0;
			}
		}

		//If no data pending in our partial qword buffer, go forward with multiple qword transfer
		if(m_directQwordBufferIndex == 0)
		{
			nSize = std::min<uint32>(m_CODE.nIMM * 0x10, nSize & ~0xF);

			auto packet = stream.GetDirectPointer();
			uint32 processed = m_gif.ProcessMultiplePackets(packet, nSize, 0, nSize, CGsPacketMetadata(2));
			assert(processed <= nSize);
			stream.Advance(processed);
			//Adjust size in case not everything was processed by GIF
			nSize = processed;

			m_CODE.nIMM -= (nSize / 0x10);
		}
	}

	if(m_CODE.nIMM == 0)
	{
		m_STAT.nVPS = 0;
	}
	else
	{
		m_STAT.nVPS = 1;
	}
}

void CVif1::Cmd_UNPACK(StreamType& stream, CODE nCommand, uint32 nDstAddr)
{
	bool nFlg = (m_CODE.nIMM & 0x8000) != 0;
	if(nFlg)
	{
		nDstAddr += m_TOPS;
	}

	return CVif::Cmd_UNPACK(stream, nCommand, nDstAddr);
}

void CVif1::PrepareMicroProgram()
{
	CVif::PrepareMicroProgram();

	m_TOP = m_TOPS;

	if(m_STAT.nDBF == 1)
	{
		m_TOPS = m_BASE;
	}
	else
	{
		m_TOPS = m_BASE + m_OFST;
	}
	m_STAT.nDBF = ~m_STAT.nDBF;
}
