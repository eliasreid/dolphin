#pragma once

#include "SlippiUtility.h"
#include "MemRegions.h"

using namespace SlippiUtility::Savestate;

// thank you Slippi :)

class BrawlbackSavestate
{

public:


    BrawlbackSavestate();
    ~BrawlbackSavestate();


    void Capture();

    void CaptureRegion(size_t index);

    void Load(std::vector<PreserveBlock> blocks);

    //static bool shouldForceInit;

    std::vector<ssBackupLoc>* getBackupLocs() { return &backupLocs; }

    int frame = -1;
    int checksum = -1;

    std::atomic_size_t helpThreadIndex = 0;
    std::atomic_size_t mainThreadIndex = 0;

private:

    std::array<u8, MemRegions::fullBackupBufferSize()> backupBuffer;

    //std::array<MemRegions::BackupRegion, 17> regionsCopy;

    std::vector<ssBackupLoc> backupLocs = {};
    std::unordered_map<PreserveBlock, std::vector<u8>, preserve_hash_fn, preserve_eq_fn> preservationMap;

    void getDolphinState(PointerWrap& p);

    void initBackupLocs();



};
