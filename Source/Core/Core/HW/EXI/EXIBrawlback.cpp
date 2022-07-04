

#include "EXIBrawlback.h"
#include "Core/ConfigManager.h"
#include "Core/HW/Memmap.h"
#include <chrono>
#include <iostream>
#include "VideoCommon/OnScreenDisplay.h"
#include <climits>
#include <fstream>
#include <filesystem>
#include <vector>
#include <regex>
#include <Core/Brawlback/include/brawlback-exi-structures/ExiStructures.h>
namespace fs = std::filesystem;

// --- Mutexes
std::mutex read_queue_mutex = std::mutex();
std::mutex remotePadQueueMutex = std::mutex();
// -------------------------------

void writeToFile(std::string filename, uint8_t* ptr, size_t len)
{
  std::ofstream fp;
  fp.open(filename, std::ios::out | std::ios::binary);
  fp.write((char*)ptr, len);
}
std::vector<u8> read_vector_from_disk(std::string file_path)
{
  std::ifstream instream(file_path, std::ios::in | std::ios::binary);
  std::vector<u8> data((std::istreambuf_iterator<char>(instream)),
                       std::istreambuf_iterator<char>());
  return data;
}

CEXIBrawlback::CEXIBrawlback()
{
  INFO_LOG(BRAWLBACK, "------- %s\n", SConfig::GetInstance().GetGameID().c_str());
#ifdef _WIN32
  if (std::filesystem::exists(Sync::getSyncLogFilePath()))
  {
    std::filesystem::remove(Sync::getSyncLogFilePath());
  }
#endif

  INFO_LOG(BRAWLBACK, "BRAWLBACK exi ctor");
  // TODO: initialize this only when finding matches
  auto enet_init_res = enet_initialize();
  if (enet_init_res < 0)
  {
    ERROR_LOG(BRAWLBACK, "Failed to init enet! %d\n", enet_init_res);
  }
  else if (enet_init_res == 0)
  {
    INFO_LOG(BRAWLBACK, "Enet init success");
  }
  this->netplay = std::make_unique<BrawlbackNetplay>();
  this->matchmaking = std::make_unique<Matchmaking>(this->getUserInfo());
  this->timeSync = std::make_unique<TimeSync>();
}

Brawlback::UserInfo CEXIBrawlback::getUserInfo()
{
  Brawlback::UserInfo info;

  std::string lylat;
#ifdef _WIN32
  std::string lylat_json_path = File::GetExeDirectory() + "/lylat.json";
#else
  // This will look on "~/Libraries/Application Support/Dolphin/lylat.json" on macosx
  std::string lylat_json_path = File::GetUserPath(D_USER_IDX) + "lylat.json";
#endif
  INFO_LOG(BRAWLBACK, "Reading lylat json from %s\n", lylat_json_path.c_str());
  std::string data;
  if (!File::ReadFileToString(lylat_json_path, data))
  {
    ERROR_LOG(BRAWLBACK, "Could not find lylat.json.");
    return info;
  }

  json j = json::parse(data);
  INFO_LOG(BRAWLBACK, "JSON Contents: %s", j.dump(4).c_str());

  info.uid = j["uid"].get<std::string>();
  info.playKey = j["playKey"].get<std::string>();
  info.connectCode = j["connectCode"].get<std::string>();
  info.displayName = "Dev Test";
  info.latestVersion = "test";
  info.fileContents = "test";

  return info;
}

CEXIBrawlback::~CEXIBrawlback()
{
  enet_deinitialize();
  enet_host_destroy(this->server);
  this->isConnected = false;
  if (this->netplay_thread.joinable())
  {
    this->netplay_thread.join();
  }

  delete this->matchmaking.release();
  if (this->matchmaking_thread.joinable())
  {
    this->matchmaking_thread.join();
  }
}

void CEXIBrawlback::handleCaptureSavestate(u8* data)
{
  // current frame we are saving (and swap endianness)
  s32 frame = (s32)SlippiUtility::Mem::readWord(data);
  SaveState(frame);
}

void CEXIBrawlback::SaveState(s32 frame)
{
  u64 startTime = Common::Timer::GetTimeUs();

  if (frame % 30 == 0)
  {
    static int dump_num = 0;
    // INFO_LOG(BRAWLBACK, "Dumping mem\n");
    // Dump::DoMemDumpIteration(dump_num);
  }

  // tmp
  if (frame == GAME_START_FRAME && availableSavestates.empty())
  {
    activeSavestates.clear();
    availableSavestates.clear();
    for (int i = 0; i <= MAX_ROLLBACK_FRAMES; i++)
    {
      availableSavestates.push_back(std::make_unique<BrawlbackSavestate>());
    }
    INFO_LOG(BRAWLBACK, "Initialized savestates!\n");
  }

  // Grab an available savestate
  std::unique_ptr<BrawlbackSavestate> ss;
  if (!availableSavestates.empty())
  {
    ss = std::move(availableSavestates.back());
    availableSavestates.pop_back();
  }
  else
  {
    // If there were no available savestates, use the oldest one
    auto it = activeSavestates.begin();
    ss = std::move(it->second);
    // s32 frameToDrop = it->first;
    // INFO_LOG(BRAWLBACK, "Dropping savestate for frame %i\n", frameToDrop);
    activeSavestates.erase(it->first);
  }

  // If there is already a savestate for this frame, remove it and add it to available
  if (!activeSavestates.empty() && activeSavestates.count(frame))
  {
    availableSavestates.push_back(std::move(activeSavestates[frame]));
    activeSavestates.erase(frame);
  }

  if (!ss)
  {
    ERROR_LOG(BRAWLBACK, "Invalid savestate on frame %i\n", frame);
    PanicAlertFmtT("Invalid savestate on frame {0}\n", frame);
    return;
  }

  ss->Capture();
  // ss->frame = frame;
  // ss->checksum = SavestateChecksum(ss->getBackupLocs()); // really expensive calculation
  // INFO_LOG(BRAWLBACK, "Savestate for frame %i checksum is %i\n", ss->frame, ss->checksum);
  activeSavestates[frame] = std::move(ss);

  u32 timeDiff = (u32)(Common::Timer::GetTimeUs() - startTime);
  INFO_LOG(BRAWLBACK, "Captured savestate for frame %d in: %f ms", frame,
           ((double)timeDiff) / 1000);
}

void CEXIBrawlback::LoadState(s32 rollbackFrame)
{
  // INFO_LOG(BRAWLBACK, "Attempting to load state for frame %i\n", frame);

  if (!activeSavestates.count(rollbackFrame))
  {
    // This savestate does not exist - just disconnect and throw hands :/
    ERROR_LOG(BRAWLBACK, "Savestate for frame %i does not exist.", rollbackFrame);
    PanicAlertFmtT("Savestate for frame {0} does not exist.", rollbackFrame);
    this->isConnected = false;
    if (this->server)
    {
      for (int i = 0; i < this->server->peerCount; i++)
      {
        enet_peer_disconnect(&this->server->peers[i], 0);
      }
    }
    // in the future, exit out of the match or something here
    return;
  }

  u64 startTime = Common::Timer::GetTimeUs();

  // Fetch preservation blocks
  std::vector<PreserveBlock> blocks = {};

  /*
// Get preservation blocks
int idx = 0;
while (Common::swap32(preserveArr[idx]) != 0)
{
  PreserveBlock p = {Common::swap32(preserveArr[idx]), Common::swap32(preserveArr[idx + 1])};
  blocks.push_back(p);
  idx += 2;
}
  */

  // Load savestate
  activeSavestates[rollbackFrame]->Load(blocks);

  // Move all active savestates to available
  for (auto it = activeSavestates.begin(); it != activeSavestates.end(); ++it)
  {
    availableSavestates.push_back(std::move(it->second));
  }

  // since we save state during resim frames, when we load state,
  // we should clear all savestates out to make room for the new resimulated states

  activeSavestates.clear();

  u32 timeDiff = (u32)(Common::Timer::GetTimeUs() - startTime);
  ERROR_LOG(BRAWLBACK, "Loaded savestate for frame %d in: %f ms", rollbackFrame, ((double)timeDiff) / 1000);
}

template <typename T>
void CEXIBrawlback::SendCmdToGame(EXICommand cmd, T* payload)
{
  // std::lock_guard<std::mutex> lock (read_queue_mutex); // crash here wtf?
  this->read_queue.clear();
  this->read_queue.push_back(cmd);
  if (payload)
  {
    std::vector<u8> byteVec = Mem::structToByteVector(payload);
    this->read_queue.insert(this->read_queue.end(), byteVec.begin(), byteVec.end());
  }
}

void CEXIBrawlback::SendCmdToGame(EXICommand cmd)
{
  // std::lock_guard<std::mutex> lock (read_queue_mutex);
  this->read_queue.clear();
  this->read_queue.push_back(cmd);
}

PlayerFrameData CreateBlankPlayerFrameData(u32 frame, u8 playerIdx)
{
  PlayerFrameData dummy_framedata;
  memset(&dummy_framedata, 0, sizeof(PlayerFrameData));
  dummy_framedata.frame = frame;
  dummy_framedata.playerIdx = playerIdx;
  dummy_framedata.pad = BrawlbackPad();  // empty pad
  return dummy_framedata;
}
FrameData CreateBlankFrameData(u32 frame) {
    FrameData fd;
    for (int i = 0; i < MAX_NUM_PLAYERS; i++) {
        fd.playerFrameDatas[i] = CreateBlankPlayerFrameData(frame, i);
    }
    return fd;
}


// `data` is a ptr to a PlayerFrameData struct
// this is called every frame at the beginning of the frame
void CEXIBrawlback::handleLocalPadData(u8* data)
{
  this->framesToAdvance = 1; // reset at beginning of frame
  FrameData frameDataFromGame;
  memcpy(&frameDataFromGame, data, sizeof(FrameData));
  SwapFrameDataEndianness(frameDataFromGame);

  PlayerFrameData localPlayerFramedata = frameDataFromGame.playerFrameDatas[localPlayerIdx];

  u32 localFrame = localPlayerFramedata.frame;
  u8 playerIdx = localPlayerIdx;

  if (localFrame == GAME_START_FRAME)
  {
    // push framedatas for first few delay frames
    for (int i = GAME_START_FRAME; i < FRAME_DELAY; i++)
    {
      this->remotePlayerFrameData[playerIdx].push_back(
          std::make_unique<PlayerFrameData>(CreateBlankPlayerFrameData(i, playerIdx)));
      this->localPlayerFrameData.push_back(
          std::make_unique<PlayerFrameData>(CreateBlankPlayerFrameData(i, playerIdx)));
    }
    this->hasGameStarted = true;
  }

    // this just for debugging purposes. Tracks and displays the number of timesyncs done every 60 frames (1 second)
    if (localFrame % 60 == 0) {
        // TPS = Timesyncs Per Second    RPS = Rollbacks Per Second
        OSD::AddTypedMessage(OSD::MessageType::NetPlayBuffer, "TPS: " + std::to_string(numTimesyncs) + "  RPS: " + std::to_string(numRollbacks) + "\n", OSD::Duration::NORMAL, OSD::Color::CYAN);
        numTimesyncs = 0;
        numRollbacks = 0;
    }

    //TODO: formatting??
    if (localFrame % PING_DISPLAY_INTERVAL == 0)
      OSD::AddTypedMessage(
          OSD::MessageType::BrawlbackBuffer,
          "Time offset: " +
              std::to_string((double)timeSync->calcTimeOffsetUs(this->numPlayers) / 1000) +
              " ms\n"); 
    

    if (resimConfirmedFrame && localFrame == *resimConfirmedFrame)
    {
      //We've resimed to the latest confirmed frame after a rollback
      resimConfirmedFrame.reset();
      //compare sync data

      for (size_t i = 0; i < this->numPlayers; i++)
      {
        if (i == localPlayerIdx)
        {
          auto* dataBeforeRollback = findInPlayerFrameDataQueue(localPlayerFrameData, localFrame);
          if (!syncDataEqual(localPlayerFramedata.syncData, dataBeforeRollback->syncData))
          {
            ERROR_LOG(BRAWLBACK, "sync data does not match after resimulation.\n");
          }
          else
          {
            ERROR_LOG(BRAWLBACK, "sync data DOES match after resimulation - rollback success\n");
          }
        }
        // remotePlayerFrameData[]
        // syncDataEqual( localPlayerFramedata.syncData,
      }
      

      //get remote frame data for this frame
      // 
      // compare localPlayerFramedata syndata to c frame data

      //localPlayerFramedata is what we JUST received from the emulator
      // data in localPlayerFramedata should be stale from before we rolled back





    }

    int remote_frame = (int)this->GetLatestRemoteFrame();
    bool shouldTimeSync = this->timeSync->shouldStallFrame(localFrame, remote_frame, this->numPlayers);
    if (shouldTimeSync)
    {
      INFO_LOG(BRAWLBACK, "Should time sync\n");
      // Send inputs that have not yet been acked
      this->handleSendInputs(localFrame);
      //this->SendCmdToGame(EXICommand::CMD_TIMESYNC);
      this->framesToAdvance = 0;
      numTimesyncs++;
    }
    else
    {
        // store these local inputs (with frame delay)
        this->storeLocalInputs(&localPlayerFramedata);
        // broadcasts local inputs
        this->handleSendInputs(localFrame);
    }
 
}

void CEXIBrawlback::handleFrameDataRequest(u8* data) {

    s32 currentFrame = (s32)SlippiUtility::Mem::readWord(data);
    INFO_LOG(BRAWLBACK, "Game requested inputs for frame %i\n", currentFrame);

    FrameData framedataToSendToGame;

    if (this->framesToAdvance != 0) {
        for (s32 i = 0; i < this->numPlayers; i++) {
            if (this->framesToAdvance == 0) {
                INFO_LOG(BRAWLBACK, "Stalling on this frame, so using blank inputs\n");
                framedataToSendToGame.playerFrameDatas[i] = CreateBlankPlayerFrameData(currentFrame, i);
                continue;
            }
            // remote inputs
            if (i != this->localPlayerIdx) {
                framedataToSendToGame.playerFrameDatas[i] = this->getRemoteInputs(currentFrame, i);
            }
        }
        // since getRemoteInputs may change the current frame (in the case of a rollback), always get local inputs last
        framedataToSendToGame.playerFrameDatas[this->localPlayerIdx] = this->getLocalInputs(currentFrame);
    }
    else {
        framedataToSendToGame = CreateBlankFrameData(currentFrame);
    }


    std::lock_guard<std::mutex> lock(read_queue_mutex);
    this->read_queue.clear();
    auto frameDataPtr = reinterpret_cast<u8*>(&framedataToSendToGame);
    this->read_queue.insert(this->read_queue.end(), frameDataPtr,
                          frameDataPtr + sizeof(FrameData));
}

PlayerFrameData CEXIBrawlback::getLocalInputs(const s32& frame) {
    const PlayerFrameData* localFrameData = findInPlayerFrameDataQueue(this->localPlayerFrameData, frame);
    if (!localFrameData || this->localPlayerFrameData.empty()) {
        // this shouldn't happen
        ERROR_LOG(BRAWLBACK, "Couldn't find local inputs! Using empty pad.\n");
        //WARN_LOG(BRAWLBACK, "Local pad input range: [%u - %u]\n", this->localPlayerFrameData.front()->frame, this->localPlayerFrameData.back()->frame);
        return CreateBlankPlayerFrameData(frame, this->localPlayerIdx);
    }
    INFO_LOG(BRAWLBACK, "Got local inputs frame = %u\n", localFrameData->frame);
    return *localFrameData;
}

void CEXIBrawlback::updateSync(s32& localFrame, u8 playerIdx) {
    // https://gist.github.com/rcmagic/f8d76bca32b5609e85ab156db38387e9#file-rollbackpseudocode-txt-L46

    s32 remoteFrame = this->GetLatestRemoteFrame();
    s32 finalFrame = MIN(remoteFrame, localFrame);
    
    bool isSynchronized = true;

    if (isPredicting && this->shouldRollback(localFrame) && latestConfirmedFrame) {

        const PlayerFrameData playerPredictedInputs = predictedInputs.playerFrameDatas[playerIdx];
        INFO_LOG(BRAWLBACK, "Checking predicted inputs against remote inputs. predicted frame = %u\n", playerPredictedInputs.frame);

        for (int i = this->latestConfirmedFrame + 1; i <= finalFrame; i++) {
            const PlayerFrameData* remoteInputs = findInPlayerFrameDataQueue(this->remotePlayerFrameData[playerIdx], i);
            if (!remoteInputs) {
                ERROR_LOG(BRAWLBACK, "Couldn't find remote inputs for frame %i in updateSync!\n", i);
            }
            else {
                if (!isInputsEqual((*remoteInputs).pad, playerPredictedInputs.pad)) {
                    // remote inputs don't match predicted
                    this->latestConfirmedFrame = i - 1;
                    isSynchronized = false;
                    INFO_LOG(BRAWLBACK, "Remote didn't match predicted inputs frame = %i\n", i);
                    break;
                }
            }
        }
    }
    else {
        INFO_LOG(BRAWLBACK, "Not predicting\n");
    }

    // remote inputs match predicted inputs or not predicting
    if (isSynchronized) {
        this->latestConfirmedFrame = finalFrame;
        INFO_LOG(BRAWLBACK, "is synchronized!\n");
    }
    else {
        // not synchronized, rollback & resim
        INFO_LOG(BRAWLBACK, "Should rollback! frame = %i latestConfirmedFrame = %i\n", localFrame, latestConfirmedFrame);
        resimConfirmedFrame = latestConfirmedFrame;
        LoadState(this->latestConfirmedFrame);
        // if on frame 10 we rollback to frame 7 we need to simulate frames 7,8,9, and 10 to get to where we were before. 10 - 7 + 1 = 4
        this->framesToAdvance = localFrame - this->latestConfirmedFrame + 1;
        INFO_LOG(BRAWLBACK, "Num frames to simulate = %u\n", framesToAdvance);
        // set the local frame to the frame we just rolled back to so we can fetch the correct inputs
        localFrame = this->latestConfirmedFrame;
        numRollbacks++;
    }

    INFO_LOG(BRAWLBACK, "UpdateSync latestConfirmedFrame = %i\n", latestConfirmedFrame);
}
bool CEXIBrawlback::shouldRollback(s32 localFrame) {
    // https://gist.github.com/rcmagic/f8d76bca32b5609e85ab156db38387e9#file-rollbackpseudocode-txt-L30
    // local_frame > sync_frame AND remote_frame > sync_frame      # No need to rollback if we don't have a frame after the previous sync frame to synchronize to.
    return localFrame > this->latestConfirmedFrame && (s32)this->GetLatestRemoteFrame() > this->latestConfirmedFrame;
}

PlayerFrameData CEXIBrawlback::getRemoteInputs(s32& localFrame, u8 playerIdx) {
    std::lock_guard<std::mutex> lock(remotePadQueueMutex);
    PlayerFrameData finalRemoteInputs;

    bool isRollbackMode = ROLLBACK_IMPL && // delay-based/rollback toggle
        localFrame > GAME_FULL_START_FRAME && // give the game a bit of time in delay-based mode to sync up
        !this->remotePlayerFrameData.empty() && // some sanity checks
        !this->remotePlayerFrameData[playerIdx].empty() &&
        this->remotePlayerFrameData[playerIdx].size() >= MAX_ROLLBACK_FRAMES;

    if (isRollbackMode) {
  
        this->updateSync(localFrame, playerIdx);

        const PlayerFrameData* remoteFrameData = findInPlayerFrameDataQueue(this->remotePlayerFrameData[playerIdx], localFrame);

        if (remoteFrameData) {
            finalRemoteInputs = *remoteFrameData;
            INFO_LOG(BRAWLBACK, "Found remote inputs frame = %u\n", finalRemoteInputs.frame);
            isPredicting = false;
        }
        else {
            INFO_LOG(BRAWLBACK, "No remote framedata!\n");
            // couldn't find remote inputs. Time to predict
            s32 predictedInputsFrame = this->latestConfirmedFrame;
            PlayerFrameData* previousInputs = findInPlayerFrameDataQueue(this->remotePlayerFrameData[playerIdx], predictedInputsFrame);
            if (!previousInputs) {
                ERROR_LOG(BRAWLBACK, "Failed to find predicted inputs for frame %i in getRemoteInputs!\n", predictedInputsFrame);
                finalRemoteInputs = CreateBlankPlayerFrameData(localFrame, playerIdx);
            }
            else {
                INFO_LOG(BRAWLBACK, "Found predicted inputs for frame = %u\n", previousInputs->frame);
                finalRemoteInputs = *previousInputs;
                predictedInputs.playerFrameDatas[playerIdx] = *previousInputs;
                isPredicting = true;
            }
        }

    }
    else {
        const PlayerFrameData* remoteFrameData = findInPlayerFrameDataQueue(this->remotePlayerFrameData[playerIdx], localFrame);
        // no rollbacks, use delay-based
        if (!remoteFrameData) {
            this->framesToAdvance = 0;
            numTimesyncs++;
            finalRemoteInputs = CreateBlankPlayerFrameData(localFrame, playerIdx);
            ERROR_LOG(BRAWLBACK, "Failed to find remote inputs in delay-based mode! Using blank framedata & stalling...\n");
        }
        else {
            INFO_LOG(BRAWLBACK, "Found remote inputs (delay-based mode)\n");
            finalRemoteInputs = *remoteFrameData;
        }
    }

    return finalRemoteInputs;
}

void CEXIBrawlback::handleFrameAdvanceRequest(u8* data) {
    // game requests the number of frames to simulate on this frame
    std::lock_guard<std::mutex> lock(read_queue_mutex);
    this->read_queue.clear();
    auto dataPtr = reinterpret_cast<u8*>(&this->framesToAdvance);
    this->read_queue.insert(this->read_queue.end(), dataPtr,
                          dataPtr + sizeof(u32));
}





void CEXIBrawlback::storeLocalInputs(PlayerFrameData* localPlayerFramedata) {
    #ifdef RANDOM_INPUTS
    if (this->localPlayerIdx == 0)
        *localPlayerFramedata = generateRandomInput(localPlayerFramedata->frame, localPlayerFramedata->playerIdx);
    #endif
    // local inputs offset by FRAME_DELAY to mask latency
    // Once we hit frame X, we send inputs for that frame, but pretend they're from frame X+2
    // so those inputs now have an extra 2 frames to get to the opponent before the opponent's
    // client hits frame X+2.
    localPlayerFramedata->frame += FRAME_DELAY;

    /*std::fstream synclogFile;
    File::OpenFStream(synclogFile, File::GetExeDirectory() + "/local_inputs.txt", std::ios_base::out | std::ios_base::app);
    synclogFile << Sync::stringifyFramedata(*localPlayerFramedata) << "\n";
    synclogFile.close();*/

    //pFD->frame += FRAME_DELAY;
    std::unique_ptr<PlayerFrameData> pFD = std::make_unique<PlayerFrameData>(*localPlayerFramedata);
    //INFO_LOG(BRAWLBACK, "Frame %u PlayerIdx: %u numPlayers %u\n", localPlayerFramedata->frame, localPlayerFramedata->playerIdx, this->numPlayers);

    // make sure we're storing inputs sequentially
    bool is_sequential_input = pFD->frame == this->localPlayerFrameData.back()->frame + 1 ||
                               this->localPlayerFrameData.empty();
    if (is_sequential_input)
    {
      // store local framedata
      if (this->localPlayerFrameData.size() > FRAMEDATA_MAX_QUEUE_SIZE)
      {
        // INFO_LOG(BRAWLBACK, "Popping local framedata for frame %u\n",
        // this->localPlayerFrameData.front()->frame);
        this->localPlayerFrameData.pop_front();
      }
      this->localPlayerFrameData.push_back(std::move(pFD));
    }
    else
    {
      // WARN_LOG(BRAWLBACK, "Didn't push local framedata for frame %u\n", pFD->frame);
    }
}

void CEXIBrawlback::handleSendInputs(u32 frame) {
    if (this->localPlayerFrameData.empty()) return;

    // broadcast this local framedata
    
    // each frame we send local inputs to the other client(s)
    // those clients then acknowledge those inputs and send that ack(nowledgement)
    // back to us. All acked inputs are irrelevant (unless we need to rollback)
    // since we know for a fact the remote client has them.
    // We track what frame of inputs has been acked ("localHeadFrame")
    // so that when we go to send inputs, we only send the un-acked ones.
    // We send *all* unacked inputs so that when the remote client doesn't receive inputs, and needs to rollback
    // the next packet will have all the inputs that that client hasn't received.
    int minAckFrame = this->timeSync->getMinAckFrame(this->numPlayers);

    // clamp to current frame to prevent it dropping local inputs that haven't been used yet
    minAckFrame = MIN(minAckFrame, frame); 


    int localPadQueueSize = (int)this->localPlayerFrameData.size();
    if (localPadQueueSize == 0) return; // no inputs, nothing to send
    int endIdx = localPadQueueSize-1 - (this->localPlayerFrameData.back()->frame - minAckFrame);

    std::vector<PlayerFrameData*> localFramedatas = {};
    // push framedatas from back to front
    // this means the resulting vector (localFramedatas) will have the most
    // recent framedata in the first position, and the oldest framedata in the last position
    for (int i = localPadQueueSize-1; i > endIdx; i--) {
        const auto& localFramedata = this->localPlayerFrameData[i];
        // make sure we queue up these inputs sequentially
        if (localFramedatas.empty() || ( !localFramedatas.empty() && localFramedatas.back()->frame > localFramedata->frame) ) {
            PlayerFrameData* inputToSend = localFramedata.get();
            localFramedatas.push_back(inputToSend);
        }
    }

    //INFO_LOG(BRAWLBACK, "Broadcasting %i framedatas\n", localFramedatas.size());
    this->netplay->BroadcastPlayerFrameDataWithPastFrames(this->server, localFramedatas);

    u32 mostRecentFrame = this->localPlayerFrameData.back()->frame; // current frame with delay
    this->timeSync->TimeSyncUpdate(mostRecentFrame, this->numPlayers);

}

u32 CEXIBrawlback::GetLatestRemoteFrame()
{
  u32 lowestFrame = 0;
  for (int i = 0; i < this->numPlayers; i++)
  {
    if (i == this->localPlayerIdx)
      continue;

    if (this->remotePlayerFrameData[i].empty())
    {
      return 0;
    }

    u32 f = this->remotePlayerFrameData[i].back()->frame;
    if (f < lowestFrame || lowestFrame == 0)
    {
      lowestFrame = f;
    }
  }

  return lowestFrame;
}

void BroadcastFramedataAck(u32 frame, u8 playerIdx, BrawlbackNetplay* netplay, ENetHost* server)
{
  FrameAck ackData;
  ackData.frame = (int)frame;
  ackData.playerIdx = playerIdx;
  sf::Packet ackDataPacket = sf::Packet();
  u8 cmdbyte = NetPacketCommand::CMD_FRAME_DATA_ACK;
  ackDataPacket.append(&cmdbyte, sizeof(cmdbyte));
  ackDataPacket.append(&ackData, sizeof(FrameAck));
  netplay->BroadcastPacket(ackDataPacket, ENET_PACKET_FLAG_UNSEQUENCED, server);
  //INFO_LOG(BRAWLBACK, "Sent ack for frame %u  pidx %u", frame, (unsigned int)playerIdx);
}

void CEXIBrawlback::ProcessIndividualRemoteFrameData(PlayerFrameData* framedata) {
    u8 playerIdx = framedata->playerIdx;
    u32 frame = framedata->frame;
    PlayerFrameDataQueue& remoteFramedataQueue = this->remotePlayerFrameData[playerIdx];

    if (!remoteFramedataQueue.empty() ) {
        // if the remote frame we're trying to process is not newer than the most recent frame, we don't care about it
        if (frame <= remoteFramedataQueue.back()->frame)
            return;
        // make sure the inputs we're adding are sequential
        if (frame != remoteFramedataQueue.back()->frame + 1) {
            ERROR_LOG(BRAWLBACK, "Remote input is not sequential! ProcessIndividualRemoteFrameData   frame = %u  remoteFrameDataQueue.back().frame+1 = %u\n", frame, remoteFramedataQueue.back()->frame+1);
            return;
        }
    }

    INFO_LOG(BRAWLBACK, "Received opponent framedata. Player %u frame: %u\n", (unsigned int)playerIdx, frame);

    std::unique_ptr<PlayerFrameData> f = std::make_unique<PlayerFrameData>(*framedata);
    remoteFramedataQueue.push_back(std::move(f));

    // clamp size of remote player framedata queue
    while (remoteFramedataQueue.size() > FRAMEDATA_MAX_QUEUE_SIZE) {
        //WARN_LOG(BRAWLBACK, "Hit remote player framedata queue max size! %u\n", remoteFramedataQueue.size());
        remoteFramedataQueue.pop_front();
    }
}

void CEXIBrawlback::ProcessRemoteFrameData(PlayerFrameData* framedatas, u8 numFramedatas_u8)
{
  s32 numFramedatas = (s32)numFramedatas_u8;
  // framedatas may point to one or more PlayerFrameData's.
  // Also note. this array is the reverse of the local pad queue, in that
  // the 0th element here is the most recent framedata.
  PlayerFrameData* mostRecentFramedata = &framedatas[0];
  u32 frame = mostRecentFramedata->frame;
  u8 playerIdx = mostRecentFramedata->playerIdx;  // remote player idx

  // acknowledge that we received opponent's framedata
  BroadcastFramedataAck(frame, playerIdx, this->netplay.get(), this->server);
  // ---------------------

  // if (!this->remotePlayerFrameData[playerIdx].empty())
  // INFO_LOG(BRAWLBACK, "Received remote inputs. Head frame %u  received head frame %u\n",
  // this->remotePlayerFrameData[playerIdx].back()->frame, frame);

  if (numFramedatas > 0)
  {
    std::lock_guard<std::mutex> lock(remotePadQueueMutex);

    std::stringstream s;
    s << "Received " << numFramedatas << " framedatas. [";
    for (int i = 0; i < numFramedatas; i++)
    {
      s << framedatas[i].frame << " , ";
    }
    s << "]";
    // INFO_LOG(BRAWLBACK, "%s\n", s.str().c_str());

    u32 maxFrame = 0;
    // index 0 is most recent, and we want to process new framedata oldest first, then newer ones
    for (s32 i = numFramedatas - 1; i >= 0; i--)
    {
      PlayerFrameData* framedata = &framedatas[i];
      maxFrame = framedata->frame > maxFrame ? framedata->frame : maxFrame;
      this->ProcessIndividualRemoteFrameData(framedata);
    }

    this->timeSync->ReceivedRemoteFramedata(maxFrame, localPlayerIdx, this->hasGameStarted);
  }
}

// called when we receive a frame ack from remote client
void CEXIBrawlback::ProcessFrameAck(FrameAck* frameAck) {
    if (frameAck->playerIdx != this->localPlayerIdx) { // should be local player
        ERROR_LOG(BRAWLBACK, "FrameAck playeridx is not local player idx! (This is wrong...)\n");
        INFO_LOG(BRAWLBACK, "FrameAck frame = %i  pidx = %i\n", frameAck->frame, (int)frameAck->playerIdx);
    }
    else {
        this->timeSync->ProcessFrameAck(frameAck);
    }
}

void CEXIBrawlback::ProcessGameSettings(GameSettings* opponentGameSettings)
{
  // merge game settings for all remote/local players, then pass that back to the game

  this->localPlayerIdx = this->isHost ? 0 : 1;
  // assumes 1v1
  int remotePlayerIdx = this->isHost ? 1 : 0;

  //doing this so functionally equivalen to what White had before
  // Probably should just use gameSettings directly...
  GameSettings& mergedGameSettings = gameSettings;
  INFO_LOG(BRAWLBACK, "ProcessGameSettings for player %u\n", this->localPlayerIdx);
  INFO_LOG(BRAWLBACK, "Remote player idx: %i\n", remotePlayerIdx);

  mergedGameSettings.localPlayerIdx = this->localPlayerIdx;

  this->numPlayers = mergedGameSettings.numPlayers;
  INFO_LOG(BRAWLBACK, "Num players from emu: %u\n", (unsigned int)this->numPlayers);

  // this is kinda broken kinda unstable and weird.
  // hardcoded "fix" for testing. Get rid of this when you're confident this is stable
  if (this->numPlayers == 0)
  {
    this->numPlayers = 2;
    mergedGameSettings.numPlayers = 2;
  }

  if (!this->isHost)
  {  // is not host
    mergedGameSettings.randomSeed = opponentGameSettings->randomSeed;

    // get a random stage on the non-host side.
    mergedGameSettings.stageID = matchmaking->GetRandomStage();

    // if not host, your character will be p2, if host, your char will be p1

    // copy char into both slots
    mergedGameSettings.playerSettings[1].charID = mergedGameSettings.playerSettings[0].charID;
    // copy char from opponent p1 into our p1
    mergedGameSettings.playerSettings[0].charID = opponentGameSettings->playerSettings[0].charID;
  }
  else
  {  // is host
    // copy char from opponent's p2 into our p2
    mergedGameSettings.playerSettings[1].charID = opponentGameSettings->playerSettings[1].charID;

    // set our stage based on the one the other client generated
    mergedGameSettings.stageID = opponentGameSettings->stageID;
  }
  mergedGameSettings.playerSettings[localPlayerIdx].playerType =
      PlayerType::PLAYERTYPE_LOCAL;
  mergedGameSettings.playerSettings[remotePlayerIdx].playerType =
      PlayerType::PLAYERTYPE_REMOTE;

  // if we're not host, we just connected to host and received their game settings,
  // now we need to send our game settings back to them so they can start their game too
  if (!this->isHost)
  {
    this->netplay->BroadcastGameSettings(this->server, &mergedGameSettings);
  }

  std::lock_guard<std::mutex> lock(read_queue_mutex);
  this->read_queue.push_back(EXICommand::CMD_SETUP_PLAYERS);
  auto gameSettingsPtr = reinterpret_cast<u8*>(&mergedGameSettings);
  this->read_queue.insert(this->read_queue.end(), gameSettingsPtr,
                          gameSettingsPtr + sizeof(GameSettings));
}

// called from netplay thread
void CEXIBrawlback::ProcessNetReceive(ENetEvent* event)
{
  ENetPacket* pckt = event->packet;
  if (pckt && pckt->data && pckt->dataLength > 0)
  {
    u8* fullpckt_data = pckt->data;
    u8 cmd_byte = fullpckt_data[0];
    u8* data = &fullpckt_data[1];

    switch (cmd_byte)
    {
    case NetPacketCommand::CMD_FRAME_DATA:
    {
      u8 numFramedatas = data[0];
      PlayerFrameData* framedata = (PlayerFrameData*)(&data[1]);
      this->ProcessRemoteFrameData(framedata, numFramedatas);
    }
    break;
    case NetPacketCommand::CMD_FRAME_DATA_ACK:
    {
      FrameAck* frameAck = (FrameAck*)data;
      this->ProcessFrameAck(frameAck);
    }
    break;
    case NetPacketCommand::CMD_GAME_SETTINGS:
    {
      INFO_LOG(BRAWLBACK, "Received game settings from opponent");
      GameSettings* gameSettingsFromOpponent = (GameSettings*)data;
      this->ProcessGameSettings(gameSettingsFromOpponent);
    }
    break;
    default:
      WARN_LOG(BRAWLBACK, "Unknown packet cmd byte!");
      INFO_LOG(BRAWLBACK, "Packet as string: %s\n", fullpckt_data);
      break;
    }
  }
}

void CEXIBrawlback::NetplayThreadFunc()
{
  Common::SetCurrentThreadName("BrawlbackNetplay");
  ENetEvent event;

  // loop until we connect to someone, then after we connected,
  // do another loop for passing data between the connected clients

  INFO_LOG(BRAWLBACK, "Waiting for connection to opponent...");
  while (enet_host_service(this->server, &event, 0) >= 0 && !this->isConnected)
  {
    switch (event.type)
    {
    case ENET_EVENT_TYPE_CONNECT:
      INFO_LOG(BRAWLBACK, "Connected!");
      if (event.peer)
      {
        INFO_LOG(BRAWLBACK, "A new client connected from %x:%u\n", event.peer->address.host,
                 event.peer->address.port);
        this->isConnected = true;
      }
      else
      {
        WARN_LOG(BRAWLBACK, "Connect event received, but peer was null!");
      }
      break;
    case ENET_EVENT_TYPE_NONE:
      // INFO_LOG(BRAWLBACK, "Enet event type none. Nothing to do");
      break;
    }
  }

  if (this->isHost)
  {  // if we're host, send our game settings to clients right after connecting
    this->netplay->BroadcastGameSettings(this->server, &this->gameSettings);
  }

  INFO_LOG(BRAWLBACK, "Starting main net data loop");
  // main enet loop
  while (enet_host_service(this->server, &event, 0) >= 0 && this->isConnected &&
         !this->timeSync->getIsConnectionStalled())
  {
    this->netplay->FlushAsyncQueue(this->server);
    switch (event.type)
    {
    case ENET_EVENT_TYPE_DISCONNECT:
      // INFO_LOG(BRAWLBACK, "%s:%u disconnected.\n", event.peer -> address.host, event.peer ->
      // address.port);
      INFO_LOG(BRAWLBACK, "disconnected.\n");
      this->isConnected = false;
      break;
    case ENET_EVENT_TYPE_NONE:
      // INFO_LOG(BRAWLBACK, "Enet event type none. Nothing to do");
      break;
    case ENET_EVENT_TYPE_RECEIVE:
      this->ProcessNetReceive(&event);
      enet_packet_destroy(event.packet);
      break;
    }
  }

  ERROR_LOG(BRAWLBACK, "~~~~~~~~~~~~~ END ENET THREAD ~~~~~~~~~~~~~~~");
}

void CEXIBrawlback::MatchmakingThreadFunc()
{
  Common::SetCurrentThreadName("BrawlbackMatchmakingPhase2");
  while (this->matchmaking)
  {
    switch (this->matchmaking->GetMatchmakeState())
    {
    case Matchmaking::ProcessState::OPPONENT_CONNECTING:
      this->matchmaking->SetMatchmakeState(Matchmaking::ProcessState::CONNECTION_SUCCESS);
      this->connectToOpponent();
      break;
    case Matchmaking::ProcessState::ERROR_ENCOUNTERED:
      ERROR_LOG(BRAWLBACK, "MATCHMAKING: ERROR TRYING TO CONNECT!");
      return;
      break;
    default:
      break;
    }
  }
  INFO_LOG(BRAWLBACK, "~~~~~~~~~~~~~~ END MATCHMAKING THREAD ~~~~~~~~~~~~~~\n");
}

void CEXIBrawlback::connectToOpponent()
{
  this->isHost = this->matchmaking->IsHost();

  if (this->isHost)
  {
    INFO_LOG(BRAWLBACK, "Matchmaking: Creating server...");
    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = this->matchmaking->GetLocalPort();

    this->server = enet_host_create(&address, 10, 3, 0, 0);
  }
  else
  {
    INFO_LOG(BRAWLBACK, "Matchmaking: Creating client...");
    this->server = enet_host_create(NULL, 10, 3, 0, 0);

    bool connectedToAtLeastOne = false;
    for (int i = 0; i < this->matchmaking->RemotePlayerCount(); i++)
    {
      ENetAddress addr;
      int set_host_res =
          enet_address_set_host(&addr, this->matchmaking->GetRemoteIPAddresses()[i].c_str());
      if (set_host_res < 0)
      {
        WARN_LOG(BRAWLBACK, "Failed to enet_address_set_host");
      }
      addr.port = this->matchmaking->GetRemotePorts()[i];

      ENetPeer* peer = enet_host_connect(this->server, &addr, 1, 0);
      if (peer == NULL)
      {
        WARN_LOG(BRAWLBACK, "Failed to enet_host_connect");
      }
      connectedToAtLeastOne = true;
    }
    if (!connectedToAtLeastOne)
    {
      ERROR_LOG(BRAWLBACK, "Failed to connect to any client/host");
      return;
    }
  }

  INFO_LOG(BRAWLBACK, "Net initialized, starting netplay thread");
  // loop to receive data over net
  this->netplay_thread = std::thread(&CEXIBrawlback::NetplayThreadFunc, this);
}

void CEXIBrawlback::handleFindMatch(u8* payload)
{
  // if (!payload) return;

#ifdef LOCAL_TESTING
  ENetAddress address;
  address.host = ENET_HOST_ANY;
  address.port = BRAWLBACK_PORT;

  this->server = enet_host_create(&address, 3, 0, 0, 0);

#define IP_FILENAME "/connect.txt"
  std::string connectIP = "127.0.0.1";

  if (File::Exists(File::GetExeDirectory() + IP_FILENAME))
  {
    std::fstream file;
    File::OpenFStream(file, File::GetExeDirectory() + IP_FILENAME, std::ios_base::in);
    connectIP.clear();
    std::getline(file, connectIP);  // read in only one line
    file.close();
    INFO_LOG(BRAWLBACK, "IP: %s\n", connectIP.c_str());
  }
  else
  {
    INFO_LOG(BRAWLBACK, "Creating connect file\n");
    std::fstream file;
    file.open(File::GetExeDirectory() + IP_FILENAME, std::ios_base::out);
    file << connectIP;
    file.close();
  }

  // just for testing. This should be replaced with a check to see if we are the "host" of the
  // match or not
  if (this->server == NULL)
  {
    this->isHost = false;
    WARN_LOG(BRAWLBACK, "Failed to init enet server!");
    WARN_LOG(BRAWLBACK, "Creating client instead...");
    this->server = enet_host_create(NULL, 3, 0, 0, 0);
    // for (int i = 0; i < 1; i++) { // make peers for all connecting opponents

    ENetAddress addr;
    int set_host_res = enet_address_set_host(&addr, connectIP.c_str());
    if (set_host_res < 0)
    {
      WARN_LOG(BRAWLBACK, "Failed to enet_address_set_host");
      return;
    }
    addr.port = BRAWLBACK_PORT;

    ENetPeer* peer = enet_host_connect(this->server, &addr, 1, 0);
    if (peer == NULL)
    {
      WARN_LOG(BRAWLBACK, "Failed to enet_host_connect");
      return;
    }

    //}
  }

  INFO_LOG(BRAWLBACK, "Net initialized, starting netplay thread");
  // loop to receive data over net
  this->netplay_thread = std::thread(&CEXIBrawlback::NetplayThreadFunc, this);
  return;
#endif

  Matchmaking::MatchSearchSettings search;
  std::string connectCode;

  // TODO: uncomment these lines when payload includes the actual mode and connect codes
#ifdef REMOVE_THIS_WHEN_PAYLOAD_IS_SET
  search.mode = (SlippiMatchmaking::OnlinePlayMode)payload[0];
  std::string shiftJisCode;
  shiftJisCode.insert(shiftJisCode.begin(), &payload[1], &payload[1] + 18);
  shiftJisCode.erase(std::find(shiftJisCode.begin(), shiftJisCode.end(), 0x00), shiftJisCode.end());
  connectCode = shiftJisCode;
#else
  search.mode = Matchmaking::OnlinePlayMode::UNRANKED;
#endif

  switch (search.mode)
  {
  case Matchmaking::OnlinePlayMode::DIRECT:
  case Matchmaking::OnlinePlayMode::TEAMS:
    search.connectCode = connectCode;
    break;
  default:
    break;
  }

  // Store this search so we know what was queued for
  lastSearch = search;
  matchmaking->FindMatch(search);
  this->matchmaking_thread = std::thread(&CEXIBrawlback::MatchmakingThreadFunc, this);
}

void CEXIBrawlback::handleStartMatch(u8* payload)
{
  // if (!payload) return;
  std::memcpy(&gameSettings, payload, sizeof(GameSettings));
}

#include <curl/curl.h>

void swapGameReportEndian(GameReport& report) {
    for (int i = 0; i < MAX_NUM_PLAYERS; i++) {
        report.damage[i] = swap_endian(report.damage[i]);
        report.stocks[i] = swap_endian(report.stocks[i]);
    }
    report.frame_duration = swap_endian(report.frame_duration);
}

void CEXIBrawlback::handleEndMatch(u8* payload) {
    GameReport report;
    memcpy(&report, payload, sizeof(GameReport));

    swapGameReportEndian(report);
	
    auto userInfo = this->matchmaking->GetUserInfo();
    INFO_LOG(BRAWLBACK, "Sending match end report\n");

    json request;
    request["uid"] = userInfo.uid;
    request["playKey"] = userInfo.playKey;
    request["gameIndex"] = gameIndex;
    request["gameDurationFrames"] = report.frame_duration;

    json players = json::array();
    for (int i = 0; i < this->numPlayers; i++)
    {
        json p;
        const Brawlback::UserInfo& playerInfo = this->matchmaking->GetPlayerInfo()[i];
        p["uid"] = playerInfo.uid;
        p["damageDone"] = report.damage[i];
        p["stocksRemaining"] = report.stocks[i];

        players[i] = p;
    }

    request["players"] = players;

    auto requestString = request.dump();

    static const std::string reportURL = "https://lylat.gg/reports";

    CURL *curl = curl_easy_init(); // TODO: init this earlier
    // Send report
    curl_easy_setopt(curl, CURLOPT_POST, true);
    curl_easy_setopt(curl, CURLOPT_URL, reportURL.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestString.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, requestString.length());
    CURLcode res = curl_easy_perform(curl);

    if (res != 0)
    {
        ERROR_LOG(BRAWLBACK, "[GameReport] Got error executing request. Err code: %d", res);
    }
    else {
        INFO_LOG(BRAWLBACK, "Successfully send end match report to %s\n", reportURL.c_str());
    }


    if (curl)
	{
		curl_easy_cleanup(curl);
	}

    gameIndex++;
}

void CEXIBrawlback::handleStartReplaysStruct(u8* payload)
{
  StartReplay startReplay;
  std::memcpy(&startReplay, payload, sizeof(StartReplay));

  auto start = this->replayJson["start"];
  for (int i = 0; i < startReplay.numPlayers; i++)
  {
    auto player = start["players"][i];
    auto replayPlayer = startReplay.players[i];
    player["ftKind"] = replayPlayer.fighterKind;
    auto position = player["startPlayerPos"];
    auto replayPosition = replayPlayer.startPlayer;

    position["x"] = replayPosition.xPos;
    position["y"] = replayPosition.yPos;
    position["z"] = replayPosition.zPos;
  }
  start["stage"] = startReplay.stage;
  start["randomSeed"] = startReplay.randomSeed;
  start["otherRandomSeed"] = startReplay.otherRandomSeed;
}

void CEXIBrawlback::handleReplaysStruct(u8* payload)
{
  Replay replay;
  std::memcpy(&replay, payload, sizeof(Replay));

  const auto frameName = fmt::format("frame_{}", replay.frameCounter);
  this->replayJson[frameName]["persistentFrameCounter"] = replay.persistentFrameCounter;
  for (int i = 0; i < replay.numItems; i++)
  {
    auto item = this->replayJson[frameName]["items"][i];
    auto replayItem = replay.items[i];

    item["itemId"] = replayItem.itemId;
    item["itemVariant"] = replayItem.itemVariant;
  }
  for (int i = 0; i < replay.numPlayers; i++)
  {
    auto player = this->replayJson[frameName]["players"][i];
    auto inputs = player["inputs"];
    auto position = player["position"];

    auto replayPlayer = replay.players[i];
    auto replayInputs = replayPlayer.inputs;
    auto replayPosition = replayPlayer.pos;

    player["actionState"] = replayPlayer.actionState;
    player["damage"] = replayPlayer.damage;
    player["stockCount"] = replayPlayer.stockCount;

    inputs["attack"] = replayInputs.attack;
    inputs["cStick"] = replayInputs.cStick;
    inputs["dTaunt"] = replayInputs.dTaunt;
    inputs["jump"] = replayInputs.jump;
    inputs["leftStickX"] = replayInputs.leftStickX;
    inputs["leftStickY"] = replayInputs.leftStickY;
    inputs["shield"] = replayInputs.shield;
    inputs["special"] = replayInputs.special;
    inputs["sTaunt"] = replayInputs.sTaunt;
    inputs["tapJump"] = replayInputs.tapJump;
    inputs["uTaunt"] = replayInputs.uTaunt;

    position["x"] = replayPosition.xPos;
    position["y"] = replayPosition.yPos;
    position["z"] = replayPosition.zPos;
  }
}

void CEXIBrawlback::handleEndOfReplay()
{
  auto ubjson = json::to_ubjson(this->replayJson);

  const auto p1 = std::chrono::system_clock::now();
  const auto timestamp =
      std::chrono::duration_cast<std::chrono::seconds>(p1.time_since_epoch()).count();
  writeToFile("replay_" + std::to_string(timestamp) + ".brba", ubjson.data(),
              ubjson.size());
}

// recieve data from game into emulator
void CEXIBrawlback::DMAWrite(u32 address, u32 size)
{
  // INFO_LOG(BRAWLBACK, "DMAWrite size: %u\n", size);
  u8* mem = Memory::GetPointer(address);

  if (!mem)
  {
    INFO_LOG(BRAWLBACK, "Invalid address in DMAWrite!");
    // this->read_queue.clear();
    return;
  }

  u8 command_byte = mem[0];  // first byte is always cmd byte
  u8* payload = &mem[1];     // rest is payload

  // no payload
  if (size <= 1)
    payload = nullptr;

  static u64 frameTime = Common::Timer::GetTimeUs();
  switch (command_byte)
  {
  case CMD_UNKNOWN:
    INFO_LOG(BRAWLBACK, "Unknown DMAWrite command byte!");
    break;
  case CMD_ONLINE_INPUTS:
    // INFO_LOG(BRAWLBACK, "DMAWrite: CMD_ONLINE_INPUTS");
    handleLocalPadData(payload);
    break;
  case CMD_FRAMEDATA:
    handleFrameDataRequest(payload);
    break;
  case CMD_FRAMEADVANCE:
    handleFrameAdvanceRequest(payload);
    break;
  case CMD_MATCH_END:
    handleEndMatch(payload);
    break;
  case CMD_CAPTURE_SAVESTATE:
    // INFO_LOG(BRAWLBACK, "DMAWrite: CMD_CAPTURE_SAVESTATE");
    handleCaptureSavestate(payload);
    break;
  case CMD_FIND_OPPONENT:
    // INFO_LOG(BRAWLBACK, "DMAWrite: CMD_FIND_OPPONENT");
    handleFindMatch(payload);
    break;
  case CMD_START_MATCH:
    // INFO_LOG(BRAWLBACK, "DMAWrite: CMD_START_MATCH");
    handleStartMatch(payload);
    break;
  case CMD_REPLAY_START_REPLAYS_STRUCT:
    handleStartReplaysStruct(payload);
    break;
  case CMD_REPLAY_REPLAYS_STRUCT:
    handleReplaysStruct(payload);
    break;
  case CMD_REPLAYS_REPLAYS_END:
    handleEndOfReplay();
    break;

  // just using these CMD's to track frame times lol
  case CMD_TIMER_START:
  {
    frameTime = Common::Timer::GetTimeUs();
  }
  break;
  case CMD_TIMER_END:
  {
    u32 timeDiff = Common::Timer::GetTimeUs() - frameTime;
    INFO_LOG(BRAWLBACK, "Game logic took %f ms\n", (double)(timeDiff / 1000.0));
  }
  break;

  default:
    // INFO_LOG(BRAWLBACK, "Default DMAWrite %u\n", (unsigned int)command_byte);
    break;
  }
}

// send data from emulator to game
void CEXIBrawlback::DMARead(u32 address, u32 size)
{
  std::lock_guard<std::mutex> lock(read_queue_mutex);

  if (this->read_queue.empty())
  {                                                       // we have nothing to send to the game
    this->read_queue.push_back(EXICommand::CMD_UNKNOWN);  // result code
  }

  // game is trying to get cmd byte (don't clear read_queue)
  if (size == 1)
  {
    Memory::CopyToEmu(address, &this->read_queue[0], size);
    this->read_queue.erase(this->read_queue.begin());
    return;
  }

  this->read_queue.resize(size, 0);
  auto qAddr = &this->read_queue[0];
  Memory::CopyToEmu(address, qAddr, size);
  this->read_queue.clear();
}

// honestly dunno why these are overriden like this, but slippi does it sooooooo  lol
bool CEXIBrawlback::IsPresent() const
{
  return true;
}

void CEXIBrawlback::TransferByte(u8& byte)
{
}
