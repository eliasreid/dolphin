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
    void CaptureFlat();
    void Load(std::vector<PreserveBlock> blocks);

    //static bool shouldForceInit;

    std::vector<ssBackupLoc>* getBackupLocs() { return &backupLocs; }

    int frame = -1;
    int checksum = -1;
private:

    std::array<u8, MemRegions::fullBackupBufferSize()> backupBuffer;

    std::vector<ssBackupLoc> backupLocs = {};
    std::unordered_map<PreserveBlock, std::vector<u8>, preserve_hash_fn, preserve_eq_fn> preservationMap;

    void getDolphinState(PointerWrap& p);

    void initBackupLocs();

    //std::thread firstHalf;
    //std::thread secondHalf;


};
