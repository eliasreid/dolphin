#include "Savestate.h"

#include <Core/HW/Memmap.h>
#include "common/Logging/Log.h"
#include <Common/MemoryUtil.h>
#include <Core/HW/EXI/EXI.h>
#include <thread>
#include "VideoCommon/OnScreenDisplay.h"
#include "Common/Thread.h"
#include "Common/Timer.h"

#include "MemRegions.h"

#define LOW_BOUND_MEM 0x80000000


// lots of code here is heavily derived from Slippi's Savestates.cpp

BrawlbackSavestate::BrawlbackSavestate()
{
    // init member list with proper addresses
    initBackupLocs();
    // iterate through address ranges and allocate mem for our savestates
    for (auto it = backupLocs.begin(); it != backupLocs.end(); ++it) {
        auto size = it->endAddress - it->startAddress;
        it->data = static_cast<u8*>(Common::AllocateAlignedMemory(size, 64));
    }
}

BrawlbackSavestate::~BrawlbackSavestate()
{
    for (auto it = backupLocs.begin(); it != backupLocs.end(); ++it)
    {
        Common::FreeAlignedMemory(it->data);
    }
}


void BrawlbackSavestate::getDolphinState(PointerWrap& p)
{
    // p.DoArray(Memory::m_pRAM, Memory::RAM_SIZE);
    // p.DoMarker("Memory");
    // VideoInterface::DoState(p);
    // p.DoMarker("VideoInterface");
    // SerialInterface::DoState(p);
    // p.DoMarker("SerialInterface");
    // ProcessorInterface::DoState(p);
    // p.DoMarker("ProcessorInterface");
    // DSP::DoState(p);
    // p.DoMarker("DSP");
    // DVDInterface::DoState(p);
    // p.DoMarker("DVDInterface");
    // GPFifo::DoState(p);
    // p.DoMarker("GPFifo");
    ExpansionInterface::DoState(p);
    p.DoMarker("ExpansionInterface");
    // AudioInterface::DoState(p);
    // p.DoMarker("AudioInterface");
}

void BrawlbackSavestate::initBackupLocs()
{
    // https://docs.google.com/spreadsheets/d/1xVvcsGZg930uVhIawacDp-brbNpLJQtzj3ry-ZQaXWo/edit?usp=sharing

    static std::vector<PreserveBlock> excludeSections = {
        // {start address, size}

        {0x935D0000, 0x935E0000-0x935D0000}, // CPP Framework heap (subject to change...??)
        // might also need the initializer sections(?) where global statics are stored
        // cpp framework code sections
        {0x817da5a4, 0x81FFFFFF-0x817da5a4},
        {0x8055A600, 0x80563100-0x8055A600},
        {0x805B5200, 0x805B61D0-0x805B5200},
        {0x817CE880, 0x817DA590-0x817CE880},
        
        {0x80663e00, 0x1a4}, // CameraController
        {0x80663b40, 0x198}, // cmAiController
        {0x805b6d20, 0x740}, // gfCameraManager
        
    };

    SlippiInitBackupLocations(this->backupLocs, memRegions, excludeSections);

    // measure total savestate size & display it
    static bool once = true;
    if (once) {
        u64 totalsize = 0;
        for (auto& loc : this->backupLocs) {
            u32 size = loc.endAddress-loc.startAddress;
            double newsize = ((double)size / 1000.0) / 1000.0;
            INFO_LOG(BRAWLBACK, "Savestate region: 0x%x - 0x%x : size %f mb   %s\n", loc.startAddress, loc.endAddress, newsize, loc.regionName.c_str());
            totalsize += size;
        }
        double dsize = ((double)totalsize / 1000.0) / 1000.0;
        std::string savestates_str = "Savestates total size: " + std::to_string(dsize) + " mb\n";
        OSD::AddTypedMessage(OSD::MessageType::Typeless, savestates_str, OSD::Duration::NORMAL, OSD::Color::GREEN);
        INFO_LOG(BRAWLBACK, "Savestates total size: %f mb\n", dsize);
    }
    once = false;
}

typedef std::vector<SlippiUtility::Savestate::ssBackupLoc>::iterator backupLocIterator;

void captureMemRegions(backupLocIterator start, backupLocIterator end) {
    for (auto it = start; it != end; ++it) {
        auto size = it->endAddress - it->startAddress;
        Memory::CopyFromEmu(it->data, it->startAddress, size);  // game -> emu
    }
}

void BrawlbackSavestate::Capture()
{
    captureMemRegions(backupLocs.begin(), backupLocs.end());
}

void BrawlbackSavestate::Load(std::vector<PreserveBlock> blocks)
{
    // Back up regions of game that should stay the same between savestates

    for (auto it = blocks.begin(); it != blocks.end(); ++it)
    {
        if (!preservationMap.count(*it)) // if this PreserveBlock is NOT in our preservationMap
        {
            // TODO: Clear preservation map when game ends
            preservationMap[*it] = std::vector<u8>(it->length); // init new entry at this PreserveBlock key
        }

        Memory::CopyFromEmu(&preservationMap[*it][0], it->address, it->length);
    }


    // Restore memory blocks
    for (auto it = backupLocs.begin(); it != backupLocs.end(); ++it)
    {
        auto size = it->endAddress - it->startAddress;
        //if (it->endAddress < LOW_BOUND_MEM)
        //{
        //    Memory::CopyToEmu(it->startAddress, it->data, it->endAddress);  // emu -> game
        //}
        //else
        //{
            Memory::CopyToEmu(it->startAddress, it->data, size);  // emu -> game
        //}
    }

    //// Restore audio
    //u8 *ptr = &dolphinSsBackup[0];
    //PointerWrap p(&ptr, PointerWrap::MODE_READ);
    //getDolphinState(p);

    // Restore preservation blocks
    for (auto it = blocks.begin(); it != blocks.end(); ++it)
    {
        Memory::CopyToEmu(it->address, &preservationMap[*it][0], it->length);
    }
  
}
