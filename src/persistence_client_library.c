/******************************************************************************
 * Project         Persistency
 * (c) copyright   2012
 * Company         XS Embedded GmbH
 *****************************************************************************/
/******************************************************************************
 * This Source Code Form is subject to the terms of the
 * Mozilla Public License, v. 2.0. If a  copy of the MPL was not distributed
 * with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
******************************************************************************/
 /**
 * @file           persistence_client_library.c
 * @ingroup        Persistence client library
 * @author         Ingo Huerner
 * @brief          Implementation of the persistence client library.
 *                 Library provides an API to access persistent data
 * @see            
 */

#include "persistence_client_library_lc_interface.h"
#include "persistence_client_library_pas_interface.h"
#include "persistence_client_library_dbus_service.h"
#include "persistence_client_library_handle.h"
#include "persistence_client_library_custom_loader.h"
#include "persistence_client_library.h"
#include "persistence_client_library_backup_filelist.h"
#include "persistence_client_library_db_access.h"
#include "persistence_client_library_dbus_cmd.h"

#if USE_FILECACHE
   #include <persistence_file_cache.h>
#endif

#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <dbus/dbus.h>
#include <pthread.h>
#include <dlt.h>
#include <dirent.h>
#include <ctype.h>


/// debug log and trace (DLT) setup
DLT_DECLARE_CONTEXT(gPclDLTContext);


/// global variable to store lifecycle shutdown mode
static int gShutdownMode = 0;
/// global shutdown cancel counter
static int gCancelCounter = 0;

static pthread_mutex_t gInitMutex = PTHREAD_MUTEX_INITIALIZER;

/// name of the backup blacklist file (contains all the files which are excluded from backup creation)
const char* gBackupFilename = "BackupFileList.info";

#if USE_APPCHECK
/// global flag
static int gAppCheckFlag = -1;
#endif

int customAsyncInitClbk(int errcode)
{
  //printf("Dummy async init Callback: %d\n", errcode);
   (void)errcode;

  return 1;
}

// forward declaration
static int private_pclInitLibrary(const char* appName, int shutdownMode);
static int private_pclDeinitLibrary(void);


/* security check for valid application:
   if the RCT table exists, the application is proven to be valid (trusted),
   otherwise return EPERS_NOPRCTABLE  */
#if USE_APPCHECK
static void doInitAppcheck(const char* appName)
{

   char rctFilename[PERS_ORG_MAX_LENGTH_PATH_FILENAME] = {0};
   snprintf(rctFilename, PERS_ORG_MAX_LENGTH_PATH_FILENAME, getLocalWtPathKey(), appName, plugin_gResTableCfg);

   if(access(rctFilename, F_OK) == 0)
   {
      gAppCheckFlag = 1;   // "trusted" application
      DLT_LOG(gPclDLTContext, DLT_LOG_INFO, DLT_STRING("initLibrary - app check: "), DLT_STRING(appName), DLT_STRING("trusted app"));
   }
   else
   {
      gAppCheckFlag = 0;   // currently not a "trusted" application
      DLT_LOG(gPclDLTContext, DLT_LOG_INFO, DLT_STRING("initLibrary - app check: "), DLT_STRING(appName), DLT_STRING("NOT trusted app"));
   }
}
#endif


#if USE_APPCHECK
int doAppcheck(void)
{
   int trusted = 1;

   if(gAppCheckFlag != 1)
   {
      char rctFilename[PERS_ORG_MAX_LENGTH_PATH_FILENAME] = {0};
      snprintf(rctFilename, PERS_ORG_MAX_LENGTH_PATH_FILENAME, getLocalWtPathKey(), gAppId, plugin_gResTableCfg);
      if(access(rctFilename, F_OK) == 0)
      {
         gAppCheckFlag = 1;   // "trusted" application
      }
      else
      {
         gAppCheckFlag = 0;   // not a "trusted" application
         trusted = 0;
      }
   }
   return trusted;
}
#endif


#define FILE_DIR_NOT_SELF_OR_PARENT(s) ((s)[0]!='.'&&(((s)[1]!='.'||(s)[2]!='\0')||(s)[1]=='\0'))


char* makeShmName(const char* path)
{
   size_t pathLen = strlen(path);
   char* result = (char*) malloc(pathLen + 1);   //free happens at lifecycle shutdown
   int i =0;

   if(result != NULL)
   {
      for(i = 0; i < pathLen; i++)
      {
         if(!isalnum(path[i]))
         {
            result[i] = '_';
         }
         else
         {
            result[i] = path[i];
         }
      }
      result[i + 1] = '\0';
   }
   else
   {
      result = NULL;
   }
   return result;
}



void checkLocalArtefacts(const char* thePath, const char* appName)
{
   struct dirent *dirent = NULL;

   if(thePath != NULL && appName != NULL)
   {
      char* name = makeShmName(appName);

      if(name != NULL)
      {
         DIR *dir = opendir(thePath);
         if(NULL != dir)
         {
            for(dirent = readdir(dir); NULL != dirent; dirent = readdir(dir))
            {
               if(FILE_DIR_NOT_SELF_OR_PARENT(dirent->d_name))
               {
                  if(strstr(dirent->d_name, name))
                  {
                     size_t len = strlen(thePath) + strlen(dirent->d_name)+1;
                     char* fileName = malloc(len);

                     if(fileName != NULL)
                     {
                        snprintf(fileName, len, "%s%s", thePath, dirent->d_name);
                        remove(fileName);

                        DLT_LOG(gPclDLTContext, DLT_LOG_WARN, DLT_STRING("pclInitLibrary => remove sem + shmem:"), DLT_STRING(fileName));
                        free(fileName);
                     }
                  }
               }
            }
            closedir(dir);
         }
         free(name);
      }
   }
}



int pclInitLibrary(const char* appName, int shutdownMode)
{
   int rval = 1;

   int lock = pthread_mutex_lock(&gInitMutex);
   if(lock == 0)
   {
      if(gPclInitCounter == 0)
      {
         DLT_REGISTER_CONTEXT(gPclDLTContext,"PCL","Ctx for PCL Logging");

         DLT_LOG(gPclDLTContext, DLT_LOG_INFO, DLT_STRING("pclInitLibrary => App:"), DLT_STRING(appName),
                                 DLT_STRING("- init counter: "), DLT_UINT(gPclInitCounter) );

         // do check if there are remaining shared memory and semaphores for local app
         checkLocalArtefacts("/dev/shm/", appName);
         //checkGroupArtefacts("/dev/shm", "group_");

         rval = private_pclInitLibrary(appName, shutdownMode);
      }
      else
      {
         DLT_LOG(gPclDLTContext, DLT_LOG_INFO, DLT_STRING("pclInitLibrary - App:"), DLT_STRING(gAppId),
                                               DLT_STRING("- ONLY INCREMENT init counter: "), DLT_UINT(gPclInitCounter) );
      }

      gPclInitCounter++;     // increment after private init, otherwise atomic access is too early
      pthread_mutex_unlock(&gInitMutex);
   }
   else
   {
     DLT_LOG(gPclDLTContext, DLT_LOG_ERROR, DLT_STRING("pclInitLibrary - mutex lock failed:"), DLT_INT(lock));
   }

   return rval;
}

static int private_pclInitLibrary(const char* appName, int shutdownMode)
{
   int rval = 1;
   char blacklistPath[PERS_ORG_MAX_LENGTH_PATH_FILENAME] = {0};
   int lock =  pthread_mutex_lock(&gDbusPendingRegMtx);   // block until pending received

   if(lock == 0)
   {
      gShutdownMode = shutdownMode;

#if USE_APPCHECK
      doInitAppcheck(appName);      // check if we have a trusted application
#endif


#if USE_FILECACHE
      DLT_LOG(gPclDLTContext, DLT_LOG_INFO, DLT_STRING("Using the filecache!!!"));
      pfcInitCache(appName);
#endif

      // Assemble backup blacklist path
      snprintf(blacklistPath, PERS_ORG_MAX_LENGTH_PATH_FILENAME, "%s%s/%s", CACHEPREFIX, appName, gBackupFilename);

      if(readBlacklistConfigFile(blacklistPath) == -1)
      {
        DLT_LOG(gPclDLTContext, DLT_LOG_INFO, DLT_STRING("initLibrary - Err access blacklist:"), DLT_STRING(blacklistPath));
      }

      if(setup_dbus_mainloop() == -1)
      {
        DLT_LOG(gPclDLTContext, DLT_LOG_ERROR, DLT_STRING("initLibrary - Failed to setup main loop"));
        pthread_mutex_unlock(&gDbusPendingRegMtx);
        return EPERS_DBUS_MAINLOOP;
      }

      if(gShutdownMode != PCL_SHUTDOWN_TYPE_NONE)
      {
        if(register_lifecycle(shutdownMode) == -1) // register for lifecycle dbus messages
        {
          DLT_LOG(gPclDLTContext, DLT_LOG_ERROR, DLT_STRING("initLibrary => Failed reg to LC dbus interface"));
          pthread_mutex_unlock(&gDbusPendingRegMtx);
          return EPERS_REGISTER_LIFECYCLE;
        }
      }
#if USE_PASINTERFACE
      DLT_LOG(gPclDLTContext, DLT_LOG_INFO, DLT_STRING("PAS interface is enabled!!"));
      if(register_pers_admin_service() == -1)
      {
        DLT_LOG(gPclDLTContext, DLT_LOG_ERROR, DLT_STRING("initLibrary - Failed reg to PAS dbus interface"));
        pthread_mutex_unlock(&gDbusPendingRegMtx);
        return EPERS_REGISTER_ADMIN;
      }
      else
      {
        DLT_LOG(gPclDLTContext, DLT_LOG_INFO,  DLT_STRING("initLibrary - Successfully established IPC protocol for PCL."));
      }
#else
      DLT_LOG(gPclDLTContext, DLT_LOG_INFO, DLT_STRING("PAS interface not enabled, enable with \"./configure --enable-pasinterface\""));
#endif

      if((rval = load_custom_plugins(customAsyncInitClbk)) < 0)      // load custom plugins
      {
        DLT_LOG(gPclDLTContext, DLT_LOG_WARN, DLT_STRING("Failed to load custom plugins"));
        pthread_mutex_unlock(&gDbusPendingRegMtx);
        return rval;
      }

      init_key_handle_array();

      pers_unlock_access();

      strncpy(gAppId, appName, PERS_RCT_MAX_LENGTH_RESPONSIBLE);  // assign application name
      gAppId[PERS_RCT_MAX_LENGTH_RESPONSIBLE-1] = '\0';

      pthread_mutex_unlock(&gDbusPendingRegMtx);
   }
   else
   {
      DLT_LOG(gPclDLTContext, DLT_LOG_ERROR, DLT_STRING("private_pclInitLibrary - mutex lock failed:"), DLT_INT(lock));
   }

   return rval;
}



int pclDeinitLibrary(void)
{
   int rval = 1;

   int lock = pthread_mutex_lock(&gInitMutex);

   if(lock == 0)
   {
      if(gPclInitCounter == 1)
      {
         DLT_LOG(gPclDLTContext, DLT_LOG_INFO, DLT_STRING("pclDeinitLibrary - DEINIT  client lib - "), DLT_STRING(gAppId),
                                               DLT_STRING("- init counter: "), DLT_UINT(gPclInitCounter));
         rval = private_pclDeinitLibrary();

         gPclInitCounter--;   // decrement init counter
         DLT_UNREGISTER_CONTEXT(gPclDLTContext);
      }
      else if(gPclInitCounter > 1)
      {
         DLT_LOG(gPclDLTContext, DLT_LOG_INFO, DLT_STRING("pclDeinitLibrary - DEINIT client lib - "), DLT_STRING(gAppId),
                                              DLT_STRING("- ONLY DECREMENT init counter: "), DLT_UINT(gPclInitCounter));
         gPclInitCounter--;   // decrement init counter
      }
      else
      {
       DLT_LOG(gPclDLTContext, DLT_LOG_WARN, DLT_STRING("pclDeinitLibrary - DEINIT client lib - "), DLT_STRING(gAppId),
                                             DLT_STRING("- NOT INITIALIZED: "));
         rval = EPERS_NOT_INITIALIZED;
      }

      pthread_mutex_unlock(&gInitMutex);
   }

   return rval;
}

static int private_pclDeinitLibrary(void)
{
   int rval = 1;
   int* retval;

   MainLoopData_u data;

   if(gShutdownMode != PCL_SHUTDOWN_TYPE_NONE)  // unregister for lifecycle dbus messages
   {
      rval = unregister_lifecycle(gShutdownMode);
   }

#if USE_PASINTERFACE == 1
   rval = unregister_pers_admin_service();
   if(0 != rval)
 {
   DLT_LOG(gPclDLTContext, DLT_LOG_ERROR, DLT_STRING("pclDeinitLibrary - Err to de-initialize IPC protocol for PCL."));
 }
   else
   {
     DLT_LOG(gPclDLTContext, DLT_LOG_INFO,  DLT_STRING("pclDeinitLibrary - Succ de-initialized IPC protocol for PCL."));
   }
#endif

   memset(&data, 0, sizeof(MainLoopData_u));
   data.cmd = (uint32_t)CMD_LC_PREPARE_SHUTDOWN;
   data.params[0] = Shutdown_Full;        // shutdown full
   data.params[1] = 0;                    // internal prepare shutdown
   data.string[0] = '\0';                 // no string parameter, set to 0
   deliverToMainloop_NM(&data);           // send quit command to dbus mainloop


   memset(&data, 0, sizeof(MainLoopData_u));
   data.cmd = (uint32_t)CMD_QUIT;
   data.string[0] = '\0';           // no string parameter, set to 0

   deliverToMainloop_NM(&data);                       // send quit command to dbus mainloop

   pthread_join(gMainLoopThread, (void**)&retval);    // wait until the dbus mainloop has ended

   deleteHandleTrees();                               // delete allocated trees
   deleteBackupTree();
   deleteNotifyTree();

   pthread_mutex_unlock(&gDbusPendingRegMtx);

#if USE_FILECACHE
   pfcDeinitCache();
#endif

   return rval;
}



int pclLifecycleSet(int shutdown)
{
   int rval = 0;

   if(gShutdownMode == PCL_SHUTDOWN_TYPE_NONE)
   {
      if(shutdown == PCL_SHUTDOWN)
      {
         MainLoopData_u data;

         DLT_LOG(gPclDLTContext, DLT_LOG_INFO, DLT_STRING("lifecycleSet - PCL_SHUTDOWN -"), DLT_STRING(gAppId));

         memset(&data, 0, sizeof(MainLoopData_u));
         data.cmd = (uint32_t)CMD_LC_PREPARE_SHUTDOWN;
         data.params[0] = Shutdown_Partial;     // shutdown partial
         data.params[1] = 0;                    // internal prepare shutdown
         data.string[0] = '\0';                 // no string parameter, set to 0
         deliverToMainloop_NM(&data);           // send quit command to dbus mainloop

         gCancelCounter++;
      }
      else if(shutdown == PCL_SHUTDOWN_CANCEL)
      {
         DLT_LOG(gPclDLTContext, DLT_LOG_INFO, DLT_STRING("lifecycleSet - PCL_SHUTDOWN_CANCEL -"), DLT_STRING(gAppId), DLT_STRING(" Cancel Counter - "), DLT_INT(gCancelCounter));
         if(gCancelCounter < Shutdown_MaxCount)
         {
           pers_unlock_access();
         }
         else
         {
           rval = EPERS_SHUTDOWN_MAX_CANCEL;
         }
      }
      else
      {
         rval = EPERS_COMMON;
      }
   }
   else
   {
      DLT_LOG(gPclDLTContext, DLT_LOG_WARN, DLT_STRING("lifecycleSet - not allowed, type not PCL_SHUTDOWN_TYPE_NONE"));
      rval = EPERS_SHUTDOWN_NO_PERMIT;
   }

   return rval;
}


#if 0
void pcl_test_send_shutdown_command()
{
   const char* command = {"snmpset -v1 -c public 134.86.58.225 iso.3.6.1.4.1.1909.22.1.1.1.5.1 i 1"};

   if(system(command) == -1)
   {
      printf("Failed to send shutdown command!!!!\n");
   }
}
#endif

