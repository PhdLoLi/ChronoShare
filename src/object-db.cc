/* -*- Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil -*- */
/*
 * Copyright(c) 2012 University of California, Los Angeles
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
 * Author: Alexander Afanasyev <alexander.afanasyev@ucla.edu>
 *	   Zhenkai Zhu <zhenkai@cs.ucla.edu>
 *	   Lijing Wang <wanglj11@mails.tsinghua.edu.cn>
 */

#include "object-db.h"
#include <iostream>
#include <boost/make_shared.hpp>
#include "db-helper.h"
#include <sys/stat.h>
#include "logging.h"

INIT_LOGGER("Object.Db");

using namespace std;
using namespace ndn;
using namespace boost;
namespace fs = boost::filesystem;

const std::string INIT_DATABASE = "\
CREATE TABLE                                                            \n \
    File(                                                               \n\
        device_name     BLOB NOT NULL,                                  \n\
        segment         INTEGER,                                        \n\
        content_object  BLOB,                                           \n\
                                                                        \
        PRIMARY KEY(device_name, segment)                              \n\
    );                                                                  \n\
CREATE INDEX device ON File(device_name);                               \n\
";

ObjectDb::ObjectDb(const fs::path &folder, const std::string &hash)
  : m_lastUsed(std::time(NULL))
{
  fs::path actualFolder = folder / "objects" / hash.substr(0, 2);
  _LOG_DEBUG("actualFolder: " << actualFolder << " hash:" << hash);
  fs::create_directories(actualFolder);

  _LOG_DEBUG("Open " <<(actualFolder / hash.substr(2, hash.size() - 2)));

  int res = sqlite3_open((actualFolder / hash.substr(2, hash.size() - 2)).c_str(), &m_db);
  if (res != SQLITE_OK)
    {
      BOOST_THROW_EXCEPTION(Error::Db()
                             << errmsg_info_str("Cannot open/create dabatabase: [" +
                                                (actualFolder / hash.substr(2, hash.size() - 2)).string() + "]"));
    }

  // Alex: determine if tables initialized. if not, initialize... not sure what is the best way to go...
  // for now, just attempt to create everything

  char *errmsg = 0;
  res = sqlite3_exec(m_db, INIT_DATABASE.c_str(), NULL, NULL, &errmsg);
  if (res != SQLITE_OK && errmsg != 0)
    {
      // _LOG_TRACE("Init \"error\": " << errmsg);
      sqlite3_free(errmsg);
    }

  // _LOG_DEBUG("open db");

  willStartSave();
}

bool
ObjectDb::DoesExist(const boost::filesystem::path &folder, const ndn::Name &deviceName, const std::string &hash)
{
  fs::path actualFolder = folder / "objects" / hash.substr(0, 2);
  bool retval = false;

  sqlite3 *db;
  int res = sqlite3_open((actualFolder / hash.substr(2, hash.size() - 2)).c_str(), &db);
  if (res == SQLITE_OK)
    {
      sqlite3_stmt *stmt;
      sqlite3_prepare_v2(db, "SELECT count(*), count(nullif (content_object,0)) FROM File WHERE device_name=?", -1, &stmt, 0);

      sqlite3_bind_blob(stmt, 1, deviceName.wireEncode().wire(), deviceName.wireEncode().size(), SQLITE_TRANSIENT);

      int res = sqlite3_step(stmt);
      if (res == SQLITE_ROW)
        {
          int countAll = sqlite3_column_int(stmt, 0);
          int countNonNull = sqlite3_column_int(stmt, 1);

          _LOG_TRACE("Total segments: " << countAll << ", non-empty segments: " << countNonNull);

          if (countAll > 0 && countAll==countNonNull)
            {
              retval = true;
            }
        }

      sqlite3_finalize(stmt);
    }

  sqlite3_close(db);
  return retval;
}


ObjectDb::~ObjectDb()
{
  didStopSave();

  // _LOG_DEBUG("close db");
  int res = sqlite3_close(m_db);
  if (res != SQLITE_OK)
    {
      // complain
    }
}

void
ObjectDb::saveContentObject(const ndn::Name &deviceName, sqlite3_int64 segment, const ndn::Data &data)
{
  sqlite3_stmt *stmt;
  sqlite3_prepare_v2(m_db, "INSERT INTO File "
                      "(device_name, segment, content_object) "
                      "VALUES(?, ?, ?)", -1, &stmt, 0);

  _LOG_DEBUG("Saving content object for [" << deviceName << ", seqno: " << segment << ", size: " << data.wireEncode().size() << "]");

  sqlite3_bind_blob(stmt, 1, deviceName.wireEncode().wire(), deviceName.wireEncode().size(), SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 2, segment);
  sqlite3_bind_blob(stmt, 3, data.wireEncode().wire(), data.wireEncode().size(), SQLITE_STATIC);

  sqlite3_step(stmt);
  //_LOG_DEBUG("After saving object: " << sqlite3_errmsg(m_db));
  sqlite3_finalize(stmt);

  // update last used time
  m_lastUsed = std::time(NULL);
}

ndn::BufferPtr
ObjectDb::fetchSegment(const ndn::Name &deviceName, sqlite3_int64 segment)
{
  sqlite3_stmt *stmt;
  sqlite3_prepare_v2(m_db, "SELECT content_object FROM File WHERE device_name=? AND segment=?", -1, &stmt, 0);

  sqlite3_bind_blob(stmt, 1, deviceName.wireEncode().wire(), deviceName.wireEncode().size(), SQLITE_TRANSIENT);

  sqlite3_bind_int64(stmt, 2, segment);

  ndn::BufferPtr ret;
  ndn::shared_ptr<Data> data = ndn::make_shared<Data>();

  int res = sqlite3_step(stmt);
  if (res == SQLITE_ROW)
    {
//      const unsigned char *buf = reinterpret_cast<const unsigned char*>(sqlite3_column_blob(stmt, 0));
      data->wireDecode(Block(sqlite3_column_blob(stmt, 0), sqlite3_column_bytes(stmt, 0)));
      ret = ndn::make_shared<ndn::Buffer>(data->getContent().value(), data->getContent().value_size());
    }

  sqlite3_finalize(stmt);

  // update last used time
  m_lastUsed = std::time(NULL);

  return ret;
}

time_t
ObjectDb::secondsSinceLastUse()
{
  return(std::time(NULL) - m_lastUsed);
}

// sqlite3_int64
// ObjectDb::getNumberOfSegments(const ndn::Name &deviceName)
// {
//   sqlite3_stmt *stmt;
//   sqlite3_prepare_v2(m_db, "SELECT count(*) FROM File WHERE device_name=?", -1, &stmt, 0);

//   bool retval = false;
//   int res = sqlite3_step(stmt);
//   if (res == SQLITE_ROW)
//     {
//       retval = true;
//     }
//   sqlite3_finalize(stmt);

//   return retval;
// }

void
ObjectDb::willStartSave()
{
  sqlite3_exec(m_db, "BEGIN TRANSACTION;", 0,0,0);
  // _LOG_DEBUG("Open transaction: " << sqlite3_errmsg(m_db));
}

void
ObjectDb::didStopSave()
{
  sqlite3_exec(m_db, "END TRANSACTION;", 0,0,0);
  // _LOG_DEBUG("Close transaction: " << sqlite3_errmsg(m_db));
}
