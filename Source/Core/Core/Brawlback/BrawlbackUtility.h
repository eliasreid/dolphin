#pragma once

#include <unordered_map>
#include <array>
#include <fstream>
#include <optional>
#include <random>

#include "Common/FileUtil.h"
#include "Common/CommonTypes.h"
#include "Common/Timer.h"
#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"
#include "SlippiUtility.h"
#include "Brawltypes.h"
#include "Savestate.h"
#include "brawlback-exi-structures/ExiStructures.h"


#define FRAME_DELAY 2
static_assert(FRAME_DELAY >= 1);
static_assert(FRAME_DELAY + MAX_ROLLBACK_FRAMES >= 6); // minimum frames of "compensation"

#define ROLLBACK_IMPL true

// number of max FrameData's to keep in the (remote) queue
#define FRAMEDATA_MAX_QUEUE_SIZE 15 
static_assert(FRAMEDATA_MAX_QUEUE_SIZE > MAX_ROLLBACK_FRAMES);
// update ping display every X frames
#define PING_DISPLAY_INTERVAL 30

// check clock desynchronization every X frames
#define ONLINE_LOCKSTEP_INTERVAL 30

#define GAME_START_FRAME 0
//#define GAME_FULL_START_FRAME 1
// before this frame we basically use delay-based netcode to ensure things are reasonably synced up before doing rollback stuff
#define GAME_FULL_START_FRAME 100

#define MAX_REMOTE_PLAYERS 3
#define MAX_NUM_PLAYERS 4
#define BRAWLBACK_PORT 7779

#define TIMESYNC_MAX_US_OFFSET 10000 // 60% of a frame

//#define SYNCLOG
//#define RANDOM_INPUTS

#define MS_IN_FRAME (1000 / 60)
#define USEC_IN_FRAME (MS_IN_FRAME*1000)
#define MS_TO_FRAMES(ms) (ms * 60 / 1000)
#define FRAMES_TO_MS(frames) (1000 * frames / 60)

// ---
// mem dumping related
#include "Core/HW/AddressSpace.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
// ---

namespace Brawlback {

    struct UserInfo
    {
      std::string uid = "";
      std::string playKey = "";
      std::string displayName = "";
      std::string connectCode = "";
      std::string latestVersion = "";
      std::string fileContents = "";

      int port;
    };

    enum EXICommand : u8
    {
      CMD_UNKNOWN = 0,

      CMD_ONLINE_INPUTS = 1,
      CMD_CAPTURE_SAVESTATE = 2,
      CMD_LOAD_SAVESTATE = 3,

      CMD_FIND_OPPONENT = 5,
      CMD_START_MATCH = 13,
      CMD_SETUP_PLAYERS = 14,
      CMD_FRAMEDATA = 15,
      CMD_TIMESYNC = 16,
      CMD_ROLLBACK = 17,

      // REPLAYS
      CMD_REPLAY_START_REPLAYS_STRUCT = 19,
      CMD_REPLAY_REPLAYS_STRUCT = 20,
      CMD_REPLAYS_REPLAYS_END = 21,

      CMD_GET_MATCH_STATE = 4,
      CMD_SET_MATCH_SELECTIONS = 6,

      CMD_TIMER_START = 7,
      CMD_TIMER_END = 8,
      CMD_UPDATE = 9,
      
      CMD_GET_ONLINE_STATUS = 10,
      CMD_CLEANUP_CONNECTION = 11,
      CMD_GET_NEW_SEED = 12,
    };

    enum NetPacketCommand : u8 
    {
        CMD_FRAME_DATA = 1,
        CMD_GAME_SETTINGS = 2,
        CMD_FRAME_DATA_ACK = 3,
    };

    struct FrameOffsetData {
        int idx;
        std::vector<s32> buf;
    };
    namespace Match
    {   
        bool isPlayerFrameDataEqual(const PlayerFrameData& p1, const PlayerFrameData& p2);
    }


    // checks if the specified `button` is held down based on the buttonBits bitfield
    bool isButtonPressed(u16 buttonBits, PADButtonBits button);
    void ResetRollbackInfo(RollbackInfo& rollbackInfo);
    namespace Mem {
        void print_byte(uint8_t byte);
        void print_half(u16 half);
        void print_word(u32 word);

        template <typename T>
        std::vector<u8> structToByteVector(T* s) {
            auto ptr = reinterpret_cast<u8*>(s);
            return std::vector<u8>(ptr, ptr + sizeof(T));
        }

        void fillByteVectorWithBuffer(std::vector<u8>& vec, u8* buf, size_t size);
    }
    namespace Sync {
        std::string getSyncLogFilePath();
        std::string str_byte(uint8_t byte);
        std::string str_half(u16 half);
        void SyncLog(const std::string& msg);
        std::string stringifyFramedata(const FrameData& fd, int numPlayers);
        std::string stringifyFramedata(const PlayerFrameData& pfd);
        std::string stringifyPad(const BrawlbackPad& pad);
    }
    
    typedef std::deque<std::unique_ptr<PlayerFrameData>> PlayerFrameDataQueue;

    PlayerFrameData* findInPlayerFrameDataQueue(const PlayerFrameDataQueue& queue,
                                                           u32 frame);

    int SavestateChecksum(std::vector<ssBackupLoc>* backupLocs);

    inline bool isInputsEqual(const BrawlbackPad& p1, const BrawlbackPad& p2) {
        bool buttons = p1.buttons == p2.buttons;
        bool triggers = p1.LTrigger == p2.LTrigger && p1.RTrigger == p2.RTrigger;
        bool analogSticks = p1.stickX == p2.stickX && p1.stickY == p2.stickY;
        bool cSticks = p1.cStickX == p2.cStickX && p1.cStickY == p2.cStickY;
        return buttons && triggers && analogSticks && cSticks;
        //return memcmp(&p1, &p2, sizeof(BrawlbackPad)) == 0;
    }

    inline PlayerFrameData generateRandomInput(s32 frame, u8 pIdx) {
        PlayerFrameData ret;
        ret.frame = frame;
        ret.playerIdx = pIdx;
        std::default_random_engine generator = std::default_random_engine((s32)Common::Timer::GetTimeUs());
        ret.pad.buttons = (u16)((generator() % 65535));
        //ret.pad.stickX = (u8)(127-generator() % (127*2));
        ret.pad.stickX = 0;
        ret.pad.stickY = (u8)(127-generator() % (127*2));
        ret.pad.cStickX = (u8)(127-generator() % (127*2));
        ret.pad.cStickY = (u8)(127-generator() % (127*2));
        ret.pad.LTrigger = (u8)(127-generator() % (127*2));
        ret.pad.RTrigger = (u8)(127-generator() % (127*2));
        return ret;
    }

    template <typename T>
    T Clamp(T input, T Max, T Min) {
        return input > Max ? Max : ( input < Min ? Min : input );
    }


    inline int MAX(int x, int y) { return (((x) > (y)) ? (x) : (y)); }
    inline int MIN(int x, int y) { return (((x) < (y)) ? (x) : (y)); }
    // 1 if in range (inclusive), 0 otherwise
    inline int RANGE(int i, int min, int max) { return ((i < min) || (i > max) ? 0 : 1); }

    namespace Dump {
        void DoMemDumpIteration(int& dump_num);
        void DumpMem(AddressSpace::Type memType, const std::string& dumpPath);
    }

}
