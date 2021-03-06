/* -*- Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2013 University of California, Los Angeles
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *	   Zhenkai Zhu <zhenkai@cs.ucla.edu>
 * Author: Alexander Afanasyev <alexander.afanasyev@ucla.edu>
 *	   Lijing Wang <wanglj11@mails.tsinghua.edu.cn>
 */

#include "dispatcher.h"
#include "logging.h"
#include "fetch-task-db.h"

#include <boost/make_shared.hpp>
#include <boost/lexical_cast.hpp>

using namespace ndn;
using namespace std;
using namespace boost;

INIT_LOGGER("Dispatcher");

static const string CHRONOSHARE_APP = "chronoshare";
static const string BROADCAST_DOMAIN = "/ndn/broadcast";

static const int CONTENT_FRESHNESS = 1800;  // seconds
const static double DEFAULT_SYNC_INTEREST_INTERVAL = 10.0; // seconds;

Dispatcher::Dispatcher(const std::string &localUserName
                       , const std::string &sharedFolder
                       , const filesystem::path &rootDir
                       , boost::shared_ptr<ndn::Face> face
                       , bool enablePrefixDiscovery
                       )
           : m_face(face)
           , m_core(NULL)
           , m_rootDir(rootDir)
           , m_executor(1) // creates problems with file assembly. need to ensure somehow that FinishExectute is called after all Segment_Execute finished
           , m_objectManager(face, rootDir, CHRONOSHARE_APP)
           , m_localUserName(localUserName)
           , m_sharedFolder(sharedFolder)
           , m_server(NULL)
           , m_enablePrefixDiscovery(enablePrefixDiscovery)
{
  // TODO check
  m_faceListening = boost::thread(boost::bind(&Dispatcher::listen, this));

  m_syncLog = boost::make_shared<SyncLog>(m_rootDir, localUserName);
  m_actionLog = boost::make_shared<ActionLog>(m_face, m_rootDir, m_syncLog, sharedFolder, CHRONOSHARE_APP,
                                       // bind(&Dispatcher::Did_ActionLog_ActionApply_AddOrModify, this, _1, _2, _3, _4, _5, _6, _7),
                                       ActionLog::OnFileAddedOrChangedCallback(), // don't really need this callback
                                       bind(&Dispatcher::Did_ActionLog_ActionApply_Delete, this, _1));
  m_fileState = m_actionLog->GetFileState();

  ndn::Name syncPrefix = ndn::Name(BROADCAST_DOMAIN);
  syncPrefix.append(CHRONOSHARE_APP);
  syncPrefix.append(sharedFolder);

  // m_server needs a different ndn face
  m_face_server = boost::make_shared<ndn::Face>();
  m_serverListening = boost::thread(boost::bind(&Dispatcher::listen_other, this, m_face_server, "contentServer"));
  m_server = new ContentServer(m_face_server, m_actionLog, rootDir, m_localUserName, m_sharedFolder, CHRONOSHARE_APP, CONTENT_FRESHNESS);
  m_server->registerPrefix(Name("/"));
  m_server->registerPrefix(Name(BROADCAST_DOMAIN));

  m_face_stateServer = boost::make_shared<ndn::Face>();
  m_stateServerListening = boost::thread(boost::bind(&Dispatcher::listen_other, this, m_face_stateServer, "stateServer"));
  m_stateServer = new StateServer(m_face_stateServer, m_actionLog, rootDir, m_localUserName, m_sharedFolder, CHRONOSHARE_APP, m_objectManager, CONTENT_FRESHNESS);
  // no need to register, right now only listening on localhop prefix

  m_core = new SyncCore(face, m_syncLog, localUserName, Name("/"), syncPrefix,
                         bind(&Dispatcher::Did_SyncLog_StateChange, this, _1), DEFAULT_SYNC_INTEREST_INTERVAL);

  FetchTaskDbPtr actionTaskDb = boost::make_shared<FetchTaskDb>(m_rootDir, "action");
  m_actionFetcher = boost::make_shared<FetchManager>(m_face, bind(&SyncLog::LookupLocator, &*m_syncLog, _1),
                                               Name(BROADCAST_DOMAIN), // no appname suffix now
                                               3,
                                               bind(&Dispatcher::Did_FetchManager_ActionFetch, this, _1, _2, _3, _4), FetchManager::FinishCallback(), actionTaskDb);

  FetchTaskDbPtr fileTaskDb = boost::make_shared<FetchTaskDb>(m_rootDir, "file");
  m_fileFetcher  = boost::make_shared<FetchManager>(m_face, bind(&SyncLog::LookupLocator, &*m_syncLog, _1),
                                              Name(BROADCAST_DOMAIN), // no appname suffix now
                                              3,
                                              bind(&Dispatcher::Did_FetchManager_FileSegmentFetch, this, _1, _2, _3, _4),
                                              bind(&Dispatcher::Did_FetchManager_FileFetchComplete, this, _1, _2),
                                              fileTaskDb);


  if (m_enablePrefixDiscovery)
  {
    _LOG_DEBUG("registering prefix discovery in Dispatcher");
    string tag = "dispatcher" + m_localUserName.toUri();
    //Ccnx::CcnxDiscovery::registerCallback(TaggedFunction(bind(&Dispatcher::Did_LocalPrefix_Updated, this, _1), tag));
    //TODO registerCallback...?
    //ndn::
    //this registerCallback is used when the local prefix changes.
    //the ndn-cxx library does not have this functionality
    //thus, the application will need to implement this.
    //send a data packet and get the local prefix. If they are different, call the callback function, else do nothing.
  }

  m_executor.start();
}

Dispatcher::~Dispatcher()
{
  m_face->shutdown();
  m_face_server->shutdown();
  m_face_stateServer->shutdown();
  
  _LOG_DEBUG("Enter destructor of dispatcher");
  m_executor.shutdown();

  // _LOG_DEBUG(">>");

  if (m_enablePrefixDiscovery)
  {
    _LOG_DEBUG("deregistering prefix discovery in Dispatcher");
    string tag = "dispatcher" + m_localUserName.toUri();
    //TODO
//    Ccnx::CcnxDiscovery::deregisterCallback(TaggedFunction(bind(&Dispatcher::Did_LocalPrefix_Updated, this, _1), tag));
  }

  if (m_core != NULL)
  {
    delete m_core;
    m_core = NULL;
  }

  if (m_server != NULL)
  {
    delete m_server;
    m_server = NULL;
  }

  if (m_stateServer != NULL)
  {
    delete m_stateServer;
    m_stateServer = NULL;
  }
}

void
Dispatcher::Did_LocalPrefix_Updated(const ndn::Name &forwardingHint)
{
  Name effectiveForwardingHint;
  if (m_localUserName.size() >= forwardingHint.size() &&
      m_localUserName.getSubName(0, forwardingHint.size()) == forwardingHint)
    {
      effectiveForwardingHint = Name("/"); // "directly" accesible
    }
  else
    {
      effectiveForwardingHint = forwardingHint;
    }

  Name oldLocalPrefix = m_syncLog->LookupLocalLocator();

  if (oldLocalPrefix == effectiveForwardingHint)
    {
      _LOG_DEBUG("Got notification about prefix change from " << oldLocalPrefix << " to: " << forwardingHint << ", but effective prefix didn't change");
      return;
    }

  if (effectiveForwardingHint == Name("/") ||
      effectiveForwardingHint == Name(BROADCAST_DOMAIN))
    {
      _LOG_DEBUG("Basic effective prefix [" << effectiveForwardingHint << "]. Updating local prefix, but don't reregister");
      m_syncLog->UpdateLocalLocator(effectiveForwardingHint);
      return;
    }

  _LOG_DEBUG("LocalPrefix changed from: " << oldLocalPrefix << " to: " << effectiveForwardingHint);

  m_server->registerPrefix(effectiveForwardingHint);
  m_syncLog->UpdateLocalLocator(effectiveForwardingHint);

  if (oldLocalPrefix == Name("/") ||
      oldLocalPrefix == Name(BROADCAST_DOMAIN))
    {
      _LOG_DEBUG("Don't deregister basic prefix: " << oldLocalPrefix);
      return;
    }
  m_server->deregisterPrefix(oldLocalPrefix);
}

// moved to state-server
// void
// Dispatcher::Restore_LocalFile(FileItemPtr file)
// {
//   m_executor.execute(bind(&Dispatcher::Restore_LocalFile_Execute, this, file));
// }

/////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////

void
Dispatcher::Did_LocalFile_AddOrModify(const filesystem::path &relativeFilePath)
{
  m_executor.execute(bind(&Dispatcher::Did_LocalFile_AddOrModify_Execute, this, relativeFilePath));
}

void
Dispatcher::Did_LocalFile_AddOrModify_Execute(filesystem::path relativeFilePath)
{
  _LOG_DEBUG(m_localUserName << " calls LocalFile_AddOrModify_Execute");
  filesystem::path absolutePath = m_rootDir / relativeFilePath;
  _LOG_DEBUG("relativeFilePath : " << relativeFilePath);
  _LOG_DEBUG("absolutePath : " << absolutePath);
  if (!filesystem::exists(absolutePath))
    {
      //BOOST_THROW_EXCEPTION(Error::Dispatcher() << error_info_str("Update non exist file: " + absolutePath.string() ));
      _LOG_DEBUG("Update non exist file: " << absolutePath.string());
      return;
    }

  FileItemPtr currentFile = m_fileState->LookupFile(relativeFilePath.generic_string());

  if (currentFile &&
      *m_digestComputer.digestFromFile(absolutePath) == ndn::Buffer(currentFile->file_hash().c_str(), currentFile->file_hash().size())
      // The following two are commented out to prevent front end from reporting intermediate files
      // should enable it if there is other way to prevent this
      // && last_write_time(absolutePath) == currentFile->mtime()
      // && status(absolutePath).permissions() == static_cast<filesystem::perms>(currentFile->mode())
      )
    {
      _LOG_ERROR("Got notification about the same file [" << relativeFilePath << "]");
      return;
    }

  if (currentFile &&
      !currentFile->is_complete())
    {
      _LOG_ERROR("Got notification about incomplete file [" << relativeFilePath << "]");
      return;
    }


  int seg_num;
  ndn::ConstBufferPtr hash;
  _LOG_DEBUG("absolutePath: " << absolutePath << " m_localUserName: " << m_localUserName);
  tie(hash, seg_num) = m_objectManager.localFileToObjects(absolutePath, m_localUserName);

  try
    {
      m_actionLog->AddLocalActionUpdate(relativeFilePath.generic_string(),
                                         *hash,
                                         last_write_time(absolutePath), 
#if BOOST_VERSION >= 104900
                                         status(absolutePath).permissions(), 
#else
                                         0,
#endif
                                         seg_num);

      // notify SyncCore to propagate the change
      m_core->localStateChangedDelayed();
    }
  catch(filesystem::filesystem_error &error)
    {
      _LOG_ERROR("File operations failed on [" << relativeFilePath << "](ignoring)");
    }

  _LOG_DEBUG("LocalFile_AddOrModify_Execute Finished!");

}

void
Dispatcher::Did_LocalFile_Delete(const filesystem::path &relativeFilePath)
{
  m_executor.execute(bind(&Dispatcher::Did_LocalFile_Delete_Execute, this, relativeFilePath));
}

void
Dispatcher::Did_LocalFile_Delete_Execute(filesystem::path relativeFilePath)
{
  filesystem::path absolutePath = m_rootDir / relativeFilePath;
  if (filesystem::exists(absolutePath))
    {
      //BOOST_THROW_EXCEPTION(Error::Dispatcher() << error_info_str("Delete notification but file exists: " + absolutePath.string() ));
      _LOG_ERROR("DELETE command, but file still exists: " << absolutePath.string());
      return;
    }

  m_actionLog->AddLocalActionDelete(relativeFilePath.generic_string());
  // notify SyncCore to propagate the change
  m_core->localStateChangedDelayed();
}

/////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * Callbacks:
 *
 * - from SyncLog: when state changes -> to fetch missing actions
 *
 * - from FetchManager/Actions: when action is fetched -> to request a file, specified by the action
 *                                                     -> to add action to the action log
 *
 * - from ActionLog/Delete:      when action applied(file state changed, file deleted)           -> to delete local file
 *
 * - from ActionLog/AddOrUpdate: when action applied(file state changes, file added or modified) -> do nothing?
 *
 * - from FetchManager/Files: when file segment is retrieved -> save it in ObjectDb
 *                            when file fetch is completed   -> if file belongs to FileState, then assemble it to filesystem. Don't do anything otherwise
 */

void
Dispatcher::Did_SyncLog_StateChange(SyncStateMsgPtr stateMsg)
{
  m_executor.execute(bind(&Dispatcher::Did_SyncLog_StateChange_Execute, this, stateMsg));
}

void
Dispatcher::Did_SyncLog_StateChange_Execute(SyncStateMsgPtr stateMsg)
{
  int size = stateMsg->state_size();
  int index = 0;
  // iterate and fetch the actions
  for (; index < size; index++)
  {
    SyncState state = stateMsg->state(index);
    if (state.has_old_seq() && state.has_seq())
    {
      uint64_t oldSeq = state.old_seq();
      uint64_t newSeq = state.seq();
      Name userName(ndn::Block((const unsigned char *)state.name().c_str(), state.name().size()));

      // fetch actions with oldSeq + 1 to newSeq(inclusive)
      ndn::Name actionNameBase = ndn::Name("/");
      actionNameBase.append(userName).append(CHRONOSHARE_APP).append("action").append(m_sharedFolder);
      m_actionFetcher->Enqueue(userName, actionNameBase,
                                std::max<uint64_t>(oldSeq + 1, 1), newSeq, FetchManager::PRIORITY_HIGH);
    }
  }
}


void
Dispatcher::Did_FetchManager_ActionFetch(const ndn::Name &deviceName, const ndn::Name &actionBaseName, uint32_t seqno, ndn::shared_ptr<ndn::Data> actionData)
{
  /// @todo Errors and exception checking
  _LOG_DEBUG("Received action deviceName: " << deviceName << ", actionBaseName: " << actionBaseName << ", seqno: " << seqno);

  ActionItemPtr action = m_actionLog->AddRemoteAction(deviceName, seqno, actionData);
  if (!action)
    {
      _LOG_ERROR("AddRemoteAction did not insert action, ignoring");
      return;
    }
  // trigger may invoke Did_ActionLog_ActionApply_Delete or Did_ActionLog_ActionApply_AddOrModify callbacks

  if (action->action() == ActionItem::UPDATE)
    {
      ndn::ConstBufferPtr hash = ndn::make_shared<ndn::Buffer>(action->file_hash().c_str(), action->file_hash().size());

      ndn::Name fileNameBase = ndn::Name("/");
      fileNameBase.append(deviceName).append(CHRONOSHARE_APP).append("file");
//      fileNameBase.append(ndn::name::Component(hash));
      fileNameBase.appendImplicitSha256Digest(hash);

      string hashStr = DigestComputer::digestToString(*hash);
      if (ObjectDb::DoesExist(m_rootDir / ".chronoshare",  deviceName, hashStr))
        {
          _LOG_DEBUG("File already exists in the database. No need to refetch, just directly applying the action");
          Did_FetchManager_FileFetchComplete(deviceName, fileNameBase);
        }
      else
        {
          if (m_objectDbMap.find(*hash) == m_objectDbMap.end())
            {
              _LOG_DEBUG("create ObjectDb for " << DigestComputer::digestToString(*hash));
              m_objectDbMap[*hash] = boost::make_shared<ObjectDb>(m_rootDir / ".chronoshare", hashStr);
            }

          m_fileFetcher->Enqueue(deviceName, fileNameBase,
                                  0, action->seg_num() - 1, FetchManager::PRIORITY_NORMAL);
        }
    }
  // if necessary(when version number is the highest) delete will be applied through the trigger in m_actionLog->AddRemoteAction call
}

void
Dispatcher::Did_ActionLog_ActionApply_Delete(const std::string &filename)
{
  m_executor.execute(bind(&Dispatcher::Did_ActionLog_ActionApply_Delete_Execute, this, filename));
}

void
Dispatcher::Did_ActionLog_ActionApply_Delete_Execute(std::string filename)
{
  _LOG_DEBUG("Action to delete " << filename);

  filesystem::path absolutePath = m_rootDir / filename;
  try
  {
    if (filesystem::exists(absolutePath))
      {
        // need some protection from local detection of removal
        remove(absolutePath);

        // hack to remove empty parent dirs
        filesystem::path parentPath = absolutePath.parent_path();
        while (parentPath > m_rootDir)
        {
          if (filesystem::is_empty(parentPath))
          {
            filesystem::remove(parentPath);
            parentPath = parentPath.parent_path();
          }
          else
          {
            break;
          }
        }
      }
    // don't exist
  }
  catch(filesystem::filesystem_error &error)
  {
    _LOG_ERROR("File operations failed when removing [" << absolutePath << "](ignoring)");
  }
}

void
Dispatcher::Did_FetchManager_FileSegmentFetch(const ndn::Name &deviceName, const ndn::Name &fileSegmentBaseName, uint32_t segment, ndn::shared_ptr<ndn::Data> fileSegmentData)
{
  m_executor.execute(bind(&Dispatcher::Did_FetchManager_FileSegmentFetch_Execute, this, deviceName, fileSegmentBaseName, segment, fileSegmentData));
}

void
Dispatcher::Did_FetchManager_FileSegmentFetch_Execute(ndn::Name deviceName, ndn::Name fileSegmentBaseName, uint32_t segment, ndn::shared_ptr<ndn::Data> fileSegmentData)
{
  // fileSegmentBaseName:  /<device_name>/<appname>/file/<hash>

  ndn::Buffer hash(fileSegmentBaseName.get(-1).value(), fileSegmentBaseName.get(-1).value_size());

  _LOG_DEBUG("Received segment deviceName: " << deviceName << ", segmentBaseName: " << fileSegmentBaseName << ", segment: " << segment);

  // _LOG_DEBUG("Looking up objectdb for " << hash);

  map<ndn::Buffer, ObjectDbPtr>::iterator db = m_objectDbMap.find(hash);
  if (db != m_objectDbMap.end())
  {
    db->second->saveContentObject(deviceName, segment, *fileSegmentData);
  }
  else
  {
    _LOG_ERROR("no db available for this content object: " << fileSegmentBaseName << ", size: " << fileSegmentData->getContent().size());
  }

  // ObjectDb objectDb(m_rootDir / ".chronoshare", lexical_cast<string>(hash));
  // objectDb.saveContentObject(deviceName, segment, *fileSegmentData);
}

void
Dispatcher::Did_FetchManager_FileFetchComplete(const ndn::Name &deviceName, const ndn::Name &fileBaseName)
{
  m_executor.execute(bind(&Dispatcher::Did_FetchManager_FileFetchComplete_Execute, this, deviceName, fileBaseName));
}

void
Dispatcher::Did_FetchManager_FileFetchComplete_Execute(ndn::Name deviceName, ndn::Name fileBaseName)
{
  // fileBaseName:  /<device_name>/<appname>/file/<hash>

  _LOG_DEBUG("Finished fetching " << deviceName << ", fileBaseName: " << fileBaseName);

  ndn::Buffer hash(fileBaseName.get(-1).value(), fileBaseName.get(-1).value_size());

  _LOG_DEBUG("Extracted hash: " << DigestComputer::digestToString(hash));

  if (m_objectDbMap.find(hash) != m_objectDbMap.end())
  {
    // remove the db handle
    m_objectDbMap.erase(hash); // to commit write
  }
  else
  {
    _LOG_ERROR("no db available for this file: " << DigestComputer::digestToString(hash));
  }

  FileItemsPtr filesToAssemble = m_fileState->LookupFilesForHash(hash);

  for (FileItems::iterator file = filesToAssemble->begin();
       file != filesToAssemble->end();
       file++)
    {
      boost::filesystem::path filePath = m_rootDir / file->filename();

      try
        {
          if (filesystem::exists(filePath) &&
              filesystem::last_write_time(filePath) == file->mtime() &&
#if BOOST_VERSION >= 104900
              filesystem::status(filePath).permissions() == static_cast<filesystem::perms>(file->mode()) &&
#endif
              *m_digestComputer.digestFromFile(filePath) == hash)
            {
              _LOG_DEBUG("Asking to assemble a file, but file already exists on a filesystem");
              continue;
            }
        }
      catch(filesystem::filesystem_error &error)
        {
          _LOG_ERROR("File operations failed on [" << filePath << "](ignoring)");
        }

      if (ObjectDb::DoesExist(m_rootDir / ".chronoshare",  deviceName, DigestComputer::digestToString(hash)))
      {
        bool ok = m_objectManager.objectsToLocalFile(deviceName, hash, filePath);
        if (ok)
          {
            last_write_time(filePath, file->mtime());
#if BOOST_VERSION >= 104900
            permissions(filePath, static_cast<filesystem::perms>(file->mode()));
#endif

            m_fileState->SetFileComplete(file->filename());
          }
        else
          {
            _LOG_ERROR("Notified about complete fetch, but file cannot be restored from the database: [" << filePath << "]");
          }
      }
      else
      {
        _LOG_ERROR(filePath << " supposed to have all segments, but not");
        // should abort for debugging
      }
    }
}

// moved to state-server
// void
// Dispatcher::Restore_LocalFile_Execute(FileItemPtr file)
// {
//   _LOG_DEBUG("Got request to restore local file [" << file->filename() << "]");
//   // the rest will gracefully fail if object-db is missing or incomplete

//   boost::filesystem::path filePath = m_rootDir / file->filename();
//   Name deviceName(file->device_name().c_str(), file->device_name().size());
//   Hash hash(file->file_hash().c_str(), file->file_hash().size());

//   try
//     {
//       if (filesystem::exists(filePath) &&
//           filesystem::last_write_time(filePath) == file->mtime() &&
//           filesystem::status(filePath).permissions() == static_cast<filesystem::perms>(file->mode()) &&
//           *Hash::FromFileContent(filePath) == hash)
//         {
//           _LOG_DEBUG("Asking to assemble a file, but file already exists on a filesystem");
//           return;
//         }
//     }
//   catch(filesystem::filesystem_error &error)
//     {
//       _LOG_ERROR("File operations failed on [" << filePath << "](ignoring)");
//     }

//   m_objectManager.objectsToLocalFile(deviceName, hash, filePath);

//   last_write_time(filePath, file->mtime());
//   permissions(filePath, static_cast<filesystem::perms>(file->mode()));

// }
