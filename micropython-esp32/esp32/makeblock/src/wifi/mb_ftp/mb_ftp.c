/**   
 * \par Copyright (C), 2017-2018, MakeBlock
 * \brief   Driver for makeblock ftp module
 * @file    mb_ftp.c
 * @author  fftust
 * @version V1.0.0
 * @date    2017/04/10
 *
 * \par Copyright
 * This software is Copyright (C), 2012-2016, MakeBlock. Use is subject to license \n
 * conditions. The main licensing options available are GPL V2 or Commercial: \n
 *
 * \par Open Source Licensing GPL V2
 * This is the appropriate option if you want to share the source code of your \n
 * application with everyone you distribute it to, and you also want to give them \n
 * the right to share who uses it. If you wish to use this software under Open \n
 * Source Licensing, you must contribute all your source code to the open source \n
 * community in accordance with the GPL Version 2 when your application is \n
 * distributed. See http://www.gnu.org/copyleft/gpl.html
 *
 * \par Description
 * This file is a drive ftp module.
 *
 * \par Method List:
 *
 *
 * \par History:
 * <pre>
 * `<Author>`         `<Time>`        `<Version>`        `<Descr>`
 *  fftust            2017/04/10       1.0.0            build the new.
 * </pre>
 *
 */

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <ctype.h>


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task.h"
#include "nvs_flash.h"

#include "py/mpstate.h"
#include "py/runtime.h"

#include "tcpip_adapter.h"
#include "lwip/sockets.h"

#include "ff.h"
#include "diskio.h"
#include "ffconf.h"
#include "mb_fatfs/pybflash.h"
#include "mb_fatfs/drivers/sflash_diskio.h"
#include "extmod/vfs_fat.h"
#include "lib/timeutils/timeutils.h"



#include "mb_ftp/mb_ftp.h"
#include "mb_ftp/drive/socketfifo.h"
#include "mb_ftp/drive/fifo.h"




/******************************************************************************
 MACRO DEFINITION
 ******************************************************************************/
#define MB_FTP_CMD_PORT                        21
#define MB_FTP_ACTIVE_DATA_PORT                20
#define MB_FTP_PASIVE_DATA_PORT                2024


#define MB_FTP_BUFFER_SIZE                     512
#define MB_FTP_TX_RETRIES_MAX                  25
#define MB_FTP_CMD_SIZE_MAX                    6
#define MB_FTP_CMD_CLIENTS_MAX                 1
#define MB_FTP_DATA_CLIENTS_MAX                1

#define MB_FTP_MAX_PARAM_SIZE                  129  //(MICROPY_ALLOC_PATH_MAX + 1)
#define MB_FTP_UNIX_TIME_20000101              946684800
#define MB_FTP_UNIX_TIME_20150101              1420070400
#define MB_FTP_UNIX_SECONDS_180_DAYS           15552000
#define MB_FTP_DATA_TIMEOUT_MS                 5000            // 5 seconds
#define MB_FTP_SOCKETFIFO_ELEMENTS_MAX         4
//#define MB_FTP_CYCLE_TIME_MS                  (SERVERS_CYCLE_TIME_MS * 2)

/******************************************************************************
 DECLARE CONSTANTS
 ******************************************************************************/

/******************************************************************************
 DEFINE TYPES
 ******************************************************************************/
typedef enum 
{
  MB_FTP_RESULT_OK = 0,
  MB_FTP_RESULT_CONTINUE,
  MB_FTP_RESULT_FAILED
} mb_ftp_result_t;

typedef enum 
{
  MB_FTP_NOTHING_OPEN = 0,
  MB_FTP_FILE_OPEN,
  MB_FTP_DIR_OPEN
} mb_ftp_open_t;

typedef struct 
{
  bool            uservalid;
  bool            passvalid;
  bool            userloggined;
} mb_ftp_loggin_t;

typedef enum 
{
  MB_FTP_DISABLED = 0, 
  MB_FTP_CONTROL_START,
  MB_FTP_CONTROL_LISTEN,
  MB_FTP_CONTROL_CONNECTED,
  MB_FTP_STE_END_TRANSFER,
  MB_FTP_STE_CONTINUE_LISTING,   
  MB_FTP_STE_CONTINUE_FILE_TX,
  MB_FTP_STE_CONTINUE_FILE_RX
} mb_ftp_state_t;     

typedef enum 
{
  MB_FTP_DATATRANS_DISCONNECTED = 0,
  MB_FTP_DATATRANS_LISTEN_FOR_DATA,
  MB_FTP_DATATRANS_CONNECTED
} mb_ftp_datatrans_state_t;

typedef enum {
    MB_FTP_CLOSE_NONE = 0,
    MB_FTP_CLOSE_DATA,
    MB_FTP_CLOSE_CMD_AND_DATA,
} mb_ftp_closesocket_t;

typedef struct 
{
  uint8_t             *ftpBuffer;  
  uint32_t            ctimeout;
  union 
  {
    FF_DIR            dp;
    FIL               fp;
  };
  int32_t             sc_sd;  //server control id
  int32_t             sd_sd;  //server data id
  int32_t             cc_sd;   //client control id
  int32_t             cd_sd;   //client data id
  int32_t             dtimeout;
  uint32_t            volcount;
  uint32_t            ip_addr; //client
  uint8_t             state;   //control sta
  mb_ftp_datatrans_state_t      data_trans_state; //data tran sta
  uint8_t             txRetries; 
  uint8_t             logginRetries;
  mb_ftp_loggin_t     loggin;
  uint8_t             e_open;
  bool                closechild;
  bool                enabled;
  bool                special_file;
  bool                listroot;
} mb_ftp_manager_t;


typedef enum {
    MB_FTP_CMD_NOT_SUPPORTED = -1,
    MB_FTP_CMD_FEAT = 0,
    MB_FTP_CMD_SYST,
    MB_FTP_CMD_CDUP,
    MB_FTP_CMD_CWD,
    MB_FTP_CMD_PWD,
    MB_FTP_CMD_XPWD,
    MB_FTP_CMD_SIZE,
    MB_FTP_CMD_MDTM,
    MB_FTP_CMD_TYPE,
    MB_FTP_CMD_USER,
    MB_FTP_CMD_PASS,
    MB_FTP_CMD_PASV,
    MB_FTP_CMD_LIST,
    MB_FTP_CMD_RETR,
    MB_FTP_CMD_STOR,
    MB_FTP_CMD_DELE,
    MB_FTP_CMD_RMD,
    MB_FTP_CMD_MKD,
    MB_FTP_CMD_RNFR,
    MB_FTP_CMD_RNTO,
    MB_FTP_CMD_NOOP,
    MB_FTP_CMD_QUIT,
    MB_FTP_NUM_FTP_CMDS
}mb_ftp_cmd_index_t;



typedef char* mb_ftp_cmdchars_t;
typedef char* mb_ftp_month_t;

static mb_ftp_manager_t mb_ftp_manager;
static char *mb_ftp_path;
static char *mb_ftp_scratch_buffer;
static char *mb_ftp_cmd_buffer;
static const mb_ftp_cmdchars_t mb_ftp_cmd_table[] = {"FEAT" ,  "SYST" ,  "CDUP" ,  "CWD" ,
                                                     "PWD"  ,  "XPWD" ,  "SIZE" ,  "MDTM" ,
                                                     "TYPE" ,  "USER" ,  "PASS" ,  "PASV" ,
                                                     "LIST" ,  "RETR" ,  "STOR" ,  "DELE" ,
                                                     "RMD"  ,  "MKD"  ,  "RNFR" ,  "RNTO" ,
                                                     "NOOP" ,  "QUIT"  };
static const mb_ftp_month_t mb_ftp_month[] = {  "Jan" ,  "Feb" ,  "Mar" ,  "Apr" ,
                                          "May" ,  "Jun" ,  "Jul" ,  "Ago" ,
                                          "Sep" ,  "Oct" ,  "Nov" ,  "Dec"  };


static const char mb_ftp_user[]={"makeblock"};   
static const char mb_ftp_pass[]={"12345678"};

static SocketFifoElement_t mb_ftp_fifoelements[MB_FTP_SOCKETFIFO_ELEMENTS_MAX];
static FIFO_t mb_ftp_socketfifo;

/******************************************************************************
 DEFINE VFS WRAPPER FUNCTIONS
 ******************************************************************************/

// These wrapper functions are used so that the FTP server can access the
// mounted FATFS devices directly without going through the costly mp_vfs_XXX
// functions.  The latter may raise exceptions and we would then need to wrap
// all calls in an nlr handler.  The wrapper functions below assume that there
// are only FATFS filesystems mounted.

STATIC FATFS *lookup_path(const TCHAR **path) {
    mp_vfs_mount_t *fs = mp_vfs_lookup_path(*path, path);
    if (fs == MP_VFS_NONE || fs == MP_VFS_ROOT) {
        return NULL;
    }
    // here we assume that the mounted device is FATFS
    return &((fs_user_mount_t*)MP_OBJ_TO_PTR(fs->obj))->fatfs;
}

STATIC FRESULT f_open_helper(FIL *fp, const TCHAR *path, BYTE mode) {
    FATFS *fs = lookup_path(&path);
    if (fs == NULL) {
        return FR_NO_PATH;
    }
    return f_open(fs, fp, path, mode);
}

STATIC FRESULT f_opendir_helper(FF_DIR *dp, const TCHAR *path) {
    FATFS *fs = lookup_path(&path);
    if (fs == NULL) {
        return FR_NO_PATH;
    }
    return f_opendir(fs, dp, path);
}

STATIC FRESULT f_stat_helper(const TCHAR *path, FILINFO *fno) {
    FATFS *fs = lookup_path(&path);
    if (fs == NULL) {
        return FR_NO_PATH;
    }
    return f_stat(fs, path, fno);
}

STATIC FRESULT f_mkdir_helper(const TCHAR *path) {
    FATFS *fs = lookup_path(&path);
    if (fs == NULL) {
        return FR_NO_PATH;
    }
    return f_mkdir(fs, path);
}

STATIC FRESULT f_unlink_helper(const TCHAR *path) {
    FATFS *fs = lookup_path(&path);
    if (fs == NULL) {
        return FR_NO_PATH;
    }
    return f_unlink(fs, path);
}

STATIC FRESULT f_rename_helper(const TCHAR *path_old, const TCHAR *path_new) {
    FATFS *fs_old = lookup_path(&path_old);
    if (fs_old == NULL) {
        return FR_NO_PATH;
    }
    FATFS *fs_new = lookup_path(&path_new);
    if (fs_new == NULL) {
        return FR_NO_PATH;
    }
    if (fs_old != fs_new) {
        return FR_NO_PATH;
    }
    return f_rename(fs_new, path_old, path_new);
}

/******************************************************************************/



STATIC void mb_ftp_close_socket(int32_t *sd);
STATIC void mb_ftp_enabled_check(void);
STATIC mb_ftp_result_t mb_ftp_send_data_non_blocking (int32_t sd, void *data, int32_t Len);
STATIC void mb_ftp_send_reply(uint32_t status, char *message);
STATIC bool mb_ftp_create_controlanddata_socket(int32_t *sd, uint32_t port);
STATIC mb_ftp_result_t mb_ftp_wait_for_connection (int32_t l_sd, int32_t *n_sd, uint32_t *ip_addr);
STATIC mb_ftp_result_t mb_ftp_receive_data(int32_t sd, void *buff, int32_t Maxlen, int32_t *rxLen);
STATIC void mb_ftp_cmd_process();
STATIC void mb_ftp_close_socket(int32_t *sd);
STATIC void mb_ftp_close_client_sockets (void);
STATIC void mb_ftp_close_files (void);




/**********************************************************************************/


void mb_ftp_enable(void)
{
   mb_ftp_manager.enabled = true;
}

void mb_ftp_disable (void) 
{
  mb_ftp_reset();
  mb_ftp_manager.enabled = false;
  mb_ftp_manager.state = MB_FTP_DISABLED;
}



void  mb_ftp_reset(void)
{
  mb_ftp_close_socket(&mb_ftp_manager.sc_sd);
  mb_ftp_close_socket(&mb_ftp_manager.sd_sd);
  mb_ftp_close_client_sockets();
  mb_ftp_manager.state = MB_FTP_CONTROL_START;
  mb_ftp_manager.data_trans_state = MB_FTP_DATATRANS_DISCONNECTED;
  mb_ftp_manager.volcount = 0;
  SOCKETFIFO_Flush();
}

STATIC void mb_ftp_enabled_check(void)
{
  if (mb_ftp_manager.enabled) 
  {
    mb_ftp_manager.state = MB_FTP_CONTROL_START;
  }
}

STATIC void mb_ftp_close_socket(int32_t *sd)
{
  if (*sd > 0)
  {
	closesocket(*sd);
	*sd = -1;
  }
}



STATIC mb_ftp_result_t mb_ftp_receive_data(int32_t sd, void *buff, int32_t Maxlen, int32_t *rxLen)
{
  *rxLen = recv(sd, buff, Maxlen, 0);

  if(*rxLen > 0)
  {
    return MB_FTP_RESULT_OK;
  } 
  else if (errno != EAGAIN)
  {
    // error
    return MB_FTP_RESULT_FAILED;
  }
  return MB_FTP_RESULT_CONTINUE;
}



STATIC mb_ftp_result_t mb_ftp_send_data_non_blocking(int32_t sd, void *data, int32_t Len) 
{
  int32_t result = send(sd, data, Len, 0);
  if (result > 0)
  {
    mb_ftp_manager.txRetries = 0;
    return MB_FTP_RESULT_OK;
  } 
  else if ((MB_FTP_TX_RETRIES_MAX >= ++mb_ftp_manager.txRetries) && (errno == EAGAIN)) 
  {
    return MB_FTP_RESULT_CONTINUE;
  }
  else 
  {
    // error
    mb_ftp_reset();
    return MB_FTP_RESULT_FAILED;
	printf("makeblock:data send eroor,ftp reset ");//fftust:debug
  }
}

STATIC void mb_ftp_send_data_to_fifo (uint32_t datasize) 
{
  SocketFifoElement_t fifoelement;

  fifoelement.data = mb_ftp_manager.ftpBuffer;
  fifoelement.datasize = datasize;
  fifoelement.sd = &mb_ftp_manager.cd_sd;
  fifoelement.closesockets = MB_FTP_CLOSE_NONE;
  fifoelement.freedata = false;
  SOCKETFIFO_Push (&fifoelement);
}

STATIC void mb_ftp_send_from_fifo (void)
{
  SocketFifoElement_t fifoelement;
  if (SOCKETFIFO_Peek (&fifoelement))
  {
    int32_t _sd = *fifoelement.sd;
    if (_sd > 0)
    {
      if (MB_FTP_RESULT_OK == mb_ftp_send_data_non_blocking (_sd, fifoelement.data, fifoelement.datasize)) 
	  {
        SOCKETFIFO_Pop (&fifoelement);
        if (fifoelement.closesockets != MB_FTP_CLOSE_NONE)
		{
          mb_ftp_close_socket(&mb_ftp_manager.cd_sd); //fftust:close the client data socket
          if (fifoelement.closesockets == MB_FTP_CLOSE_CMD_AND_DATA)
		  {
            mb_ftp_close_socket(&mb_ftp_manager.sd_sd);//fftust:close the serve data socket
            // this one is the command socket
            mb_ftp_close_socket(fifoelement.sd); //fftust:????????????
            mb_ftp_manager.data_trans_state = MB_FTP_DATATRANS_DISCONNECTED;
          }
          //ftp_close_filesystem_on_error();//fftust:need to define this function:
        }
        if (fifoelement.freedata)
		{
          vPortFree(fifoelement.data);
        }
      }
    } 
    else 
	{
	  // socket closed, remove it from the queue
      SOCKETFIFO_Pop (&fifoelement);
      if (fifoelement.freedata) 
	  {
        vPortFree(fifoelement.data);
      }
    }
  } 
  else if (mb_ftp_manager.state == MB_FTP_STE_END_TRANSFER && (mb_ftp_manager.cd_sd > 0))
  {
    // close the listening and the data sockets
    mb_ftp_close_socket(&mb_ftp_manager.sd_sd);
    mb_ftp_close_socket(&mb_ftp_manager.cd_sd);
    if (mb_ftp_manager.special_file) 
    {
      mb_ftp_manager.special_file = false;
    }
  }
}

STATIC void mb_ftp_send_reply(uint32_t status, char *message)
{

  SocketFifoElement_t fifoelement;
  if (!message) 
  {
  	message = "";
  }
  snprintf((char *)mb_ftp_cmd_buffer, 4, "%u", status);
  strcat ((char *)mb_ftp_cmd_buffer, " ");  
  strcat ((char *)mb_ftp_cmd_buffer, message);
  strcat ((char *)mb_ftp_cmd_buffer, "\r\n");
  
  fifoelement.sd = &mb_ftp_manager.cc_sd;
  fifoelement.datasize = strlen((char *)mb_ftp_cmd_buffer);
  fifoelement.data = pvPortMalloc(fifoelement.datasize);
  if (status == 221) 
  {
  	fifoelement.closesockets = MB_FTP_CLOSE_CMD_AND_DATA;
  }
  else if (status == 426 || status == 451 || status == 550) 
  {
  	fifoelement.closesockets = MB_FTP_CLOSE_DATA;
  }
  else
  {
  	fifoelement.closesockets = MB_FTP_CLOSE_NONE;
  }

  
  fifoelement.freedata = true;
  if (fifoelement.data) 
  {
  	memcpy (fifoelement.data, mb_ftp_cmd_buffer, fifoelement.datasize);
  	if (!SOCKETFIFO_Push (&fifoelement)) 
	{
  		vPortFree(fifoelement.data);
  	}

  }

}


STATIC bool mb_ftp_create_controlanddata_socket(int32_t *sd, uint32_t port)
{
  struct sockaddr_in sServeAddress;
  int32_t result;
  int32_t _sd;
  // open a socket for ftp control
  *sd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  _sd=*sd;
  if (_sd > 0) 
  {
    // enable non-blocking mode
    uint32_t option = fcntl(_sd, F_GETFL, 0);
    option |= O_NONBLOCK;
    fcntl(_sd, F_SETFL, option);

    // enable address reusing
    option = 1;
    result = setsockopt(_sd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

    // bind the socket to a port number
    sServeAddress.sin_family = AF_INET;
    sServeAddress.sin_addr.s_addr = INADDR_ANY;
    sServeAddress.sin_len = sizeof(sServeAddress);
    sServeAddress.sin_port = htons(port);

    result = bind(_sd, (const struct sockaddr *)&sServeAddress, sizeof(sServeAddress));

    // start listening
    result |= listen (_sd, 0);

    if (!result) 
	{
      return true;
    }
    mb_ftp_close_socket(sd);
  }

  return false;
  
}


STATIC mb_ftp_result_t mb_ftp_wait_for_connection (int32_t l_sd, int32_t *n_sd, uint32_t *ip_addr)
{
  struct sockaddr_in  sClientAddress;
  socklen_t  in_addrSize;
  
  // accepts a connection from a TCP client, if there is any, otherwise returns EAGAIN
  *n_sd = accept(l_sd, (struct sockaddr *)&sClientAddress, (socklen_t *)&in_addrSize);
  int32_t _sd = *n_sd;
  if (_sd < 0)
  {
   if (errno == EAGAIN) 
   {
     return MB_FTP_RESULT_CONTINUE;
   }
   // error
   mb_ftp_reset();
   return MB_FTP_RESULT_FAILED;
  }
  
  if (ip_addr)
  {
    tcpip_adapter_ip_info_t ip_info;
    wifi_mode_t wifi_mode;
    esp_wifi_get_mode(&wifi_mode);
    if (wifi_mode != WIFI_MODE_APSTA)
	{
      // easy way
      tcpip_adapter_if_t if_type;
      if (wifi_mode == WIFI_MODE_AP)
	  {
        if_type = TCPIP_ADAPTER_IF_AP;
      }
	  else
	  {
        if_type = TCPIP_ADAPTER_IF_STA;
      }
      tcpip_adapter_get_ip_info(if_type, &ip_info);
    } 
	else
	{
      // see on which subnet is the client ip address
      tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_AP, &ip_info);
      if ((ip_info.ip.addr & ip_info.netmask.addr) != (ip_info.netmask.addr & sClientAddress.sin_addr.s_addr)) 
	  {
        tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info);
      }
    }
   *ip_addr = ip_info.ip.addr;
  }
  
  
  // enable non-blocking mode
  uint32_t option = fcntl(_sd, F_GETFL, 0);
  option |= O_NONBLOCK;
  fcntl(_sd, F_SETFL, option);
  
  // client connected, so go on
  return MB_FTP_RESULT_OK;
}

STATIC void mb_ftp_close_filesystem_on_error (void) 
{
    mb_ftp_close_files();
    if (mb_ftp_manager.special_file)
	{
      //updater_finnish ();
      mb_ftp_manager.special_file = false;
    }
}

STATIC void mb_ftp_close_client_sockets (void)
{
  mb_ftp_close_socket(&mb_ftp_manager.cc_sd);
  mb_ftp_close_socket(&mb_ftp_manager.cd_sd);
  mb_ftp_close_filesystem_on_error ();
}

STATIC void mb_ftp_pop_param (char **str, char *param) 
{
  while (**str != ' ' && **str != '\r' && **str != '\n' && **str != '\0')
  {
    *param++ = **str;
    (*str)++;
  }
  *param = '\0';
}

void mb_stoupper (char *str)  //fftust:case-insensitive
{
  while (str && *str != '\0') 
  {
    *str = (char)toupper((int)(*str));
    str++;
  }
}






STATIC mb_ftp_cmd_index_t mb_ftp_pop_command (char **str) 
{
  char _cmd[MB_FTP_CMD_SIZE_MAX];
  mb_ftp_pop_param (str, _cmd);
  mb_stoupper(_cmd);
  for (mb_ftp_cmd_index_t i = 0; i < MB_FTP_NUM_FTP_CMDS; i++)
  {
    if (!strcmp (_cmd, mb_ftp_cmd_table[i])) 
	{
      (*str)++;
      return i;
    }
  }
    return MB_FTP_CMD_NOT_SUPPORTED;
}

STATIC void mb_ftp_close_files (void)
{
  if (mb_ftp_manager.e_open == MB_FTP_FILE_OPEN)
  {
    f_close(&mb_ftp_manager.fp);
  } 
  else if (mb_ftp_manager.e_open == MB_FTP_DIR_OPEN)
  {
    f_closedir(&mb_ftp_manager.dp);
  }
  mb_ftp_manager.e_open = MB_FTP_NOTHING_OPEN;
}

STATIC void mb_ftp_open_child (char *pwd, char *dir)
{
  if (dir[0] == '/') 
  {
    strcpy (pwd, dir);
  }
  else
  {
    if (strlen(pwd) > 1) 
	{
      strcat (pwd, "/");
    }
    strcat (pwd, dir);
  }

  uint len = strlen(pwd);
  if ((len > 1) && (pwd[len - 1] == '/'))
  {
    pwd[len - 1] = '\0';
  }
}

STATIC void mb_ftp_close_child (char *pwd)
{
  uint len = strlen(pwd);
  while (len && (pwd[len] != '/'))
  {
    len--;
  }
  if (len == 0)
  {
    strcpy (pwd, "/");
  }
  else 
  {
    pwd[len] = '\0';
  }
}

STATIC void mb_ftp_return_to_previous_path (char *pwd, char *dir) 
{
  uint newlen = strlen(pwd) - strlen(dir);
  if ((newlen > 2) && (pwd[newlen - 1] == '/'))
  {
    pwd[newlen - 1] = '\0';
  }
  else 
  {
    if (newlen == 0)
	{
      strcpy (pwd, "/");
    }
	else
	{
      pwd[newlen] = '\0';
    }
  }
}

STATIC void mb_ftp_get_param_and_open_child (char **bufptr)
{
    mb_ftp_pop_param (bufptr, mb_ftp_scratch_buffer);
    mb_ftp_open_child (mb_ftp_path, mb_ftp_scratch_buffer);
    mb_ftp_manager.closechild = true;
}


STATIC bool mb_ftp_open_file (const char *path, int mode)
{
  FRESULT res = f_open_helper(&mb_ftp_manager.fp, path, mode);
  if (res != FR_OK)
  {
    return false;
  }
  mb_ftp_manager.e_open = MB_FTP_FILE_OPEN;
  return true;
}

STATIC  mb_ftp_result_t mb_ftp_read_file (char *filebuf, uint32_t desiredsize, uint32_t *actualsize) 
{
  mb_ftp_result_t result = MB_FTP_RESULT_CONTINUE;
  FRESULT res = f_read(&mb_ftp_manager.fp,filebuf, desiredsize, (UINT *)actualsize);
  if (res != FR_OK) 
  {
    mb_ftp_close_files();
    result = MB_FTP_RESULT_FAILED;
    *actualsize = 0;
  }
  else if (*actualsize < desiredsize) 
  {
    mb_ftp_close_files();
    result = MB_FTP_RESULT_OK;
  }
  return result;
}

STATIC mb_ftp_result_t mb_ftp_write_file (char *filebuf, uint32_t size) 
{
  mb_ftp_result_t result = MB_FTP_RESULT_FAILED;
  uint32_t actualsize;
  FRESULT res = f_write(&mb_ftp_manager.fp, filebuf, size, (UINT *)&actualsize);
  if ((actualsize == size) && (FR_OK == res))
  {
    result = MB_FTP_RESULT_OK;
  }
  else 
  {
    mb_ftp_close_files();
  }
  return result;
}

STATIC int mb_ftp_print_eplf_item (char *dest, uint32_t destsize, FILINFO *fno)
{

  char *type = (fno->fattrib & AM_DIR) ? "d" : "-";
  uint32_t tseconds;
  uint mindex = (((fno->fdate >> 5) & 0x0f) > 0) ? (((fno->fdate >> 5) & 0x0f) - 1) : 0;
  uint day = ((fno->fdate & 0x1f) > 0) ? (fno->fdate & 0x1f) : 1;
  uint fseconds = timeutils_seconds_since_2000(1980 + ((fno->fdate >> 9) & 0x7f),
                                                      (fno->fdate >> 5) & 0x0f,
                                                      fno->fdate & 0x1f,
                                                      (fno->ftime >> 11) & 0x1f,
                                                      (fno->ftime >> 5) & 0x3f,
                                                      2 * (fno->ftime & 0x1f));
  tseconds = 3600;//pyb_rtc_get_seconds();
  if (MB_FTP_UNIX_SECONDS_180_DAYS < tseconds - fseconds)
  {
    return snprintf(dest, destsize, "%srw-rw-r--   1 root  root %9u %s %2u %5u %s\r\n",
                    type, (uint32_t)fno->fsize, mb_ftp_month[mindex], day,
                    1980 + ((fno->fdate >> 9) & 0x7f), fno->fname);
  }
  else
  {
    return snprintf(dest, destsize, "%srw-rw-r--   1 root  root %9u %s %2u %02u:%02u %s\r\n",
                    type, (uint32_t)fno->fsize, mb_ftp_month[mindex], day,
                    (fno->ftime >> 11) & 0x1f, (fno->ftime >> 5) & 0x3f, fno->fname);
  }
  
}

STATIC int mb_ftp_print_eplf_drive (char *dest, uint32_t destsize, const char *name)
{ 
  timeutils_struct_time_t tm;
  uint32_t tseconds;
  char *type = "d";
  timeutils_seconds_since_2000_to_struct_time((MB_FTP_UNIX_TIME_20150101 - MB_FTP_UNIX_TIME_20000101), &tm);
	
  tseconds = 3600;//pyb_rtc_get_seconds();
  if (MB_FTP_UNIX_SECONDS_180_DAYS < tseconds - (MB_FTP_UNIX_TIME_20150101 - MB_FTP_UNIX_TIME_20000101))
  {
    return snprintf(dest, destsize, "%srw-rw-r--   1 root  root %9u %s %2u %5u %s\r\n",
 				   type, 0, mb_ftp_month[(tm.tm_mon - 1)], tm.tm_mday, tm.tm_year, name);
  }
  else 
  {
    return snprintf(dest, destsize, "%srw-rw-r--   1 root  root %9u %s %2u %02u:%02u %s\r\n",
						   type, 0, mb_ftp_month[(tm.tm_mon - 1)], tm.tm_mday, tm.tm_hour, tm.tm_min, name);
  }

}

STATIC mb_ftp_result_t mb_ftp_list_dir (char *list, uint32_t maxlistsize, uint32_t *listsize) 
{
  uint next = 0;
  uint listcount = 0;
  FRESULT res;
  mb_ftp_result_t result = MB_FTP_RESULT_CONTINUE;
  FILINFO fno;
  #if _USE_LFN
  // read up to 2 directory items
  while (listcount < 2) {
  #else
  // read up to 4 directory items
  while (listcount < 4) {
  #endif
    if (mb_ftp_manager.listroot)
    {
      // root directory "hack"
      mp_vfs_mount_t *vfs = MP_STATE_VM(vfs_mount_table);
      int i = mb_ftp_manager.volcount;
      while (vfs != NULL && i != 0)
  	{
        vfs = vfs->next;
        i -= 1;
      }
      if (vfs == NULL)
  	{
        if (!next)
  	  {
          // no volume found this time, we are done
          mb_ftp_manager.volcount = 0;
        }
        break;
      } 
  	else
  	{
        next += mb_ftp_print_eplf_drive((list + next), (maxlistsize - next), vfs->str + 1);
      }
      mb_ftp_manager.volcount++;
    } 
    else
    {
      // a "normal" directory
      res = f_readdir(&mb_ftp_manager.dp, &fno);                                                       /* Read a directory item */
  		if (res != FR_OK || fno.fname[0] == 0) 
  	{
        result = MB_FTP_RESULT_OK;
        break;                                                                                 /* Break on error or end of dp */
      }
      if (fno.fname[0] == '.' && fno.fname[1] == 0) continue;                                    /* Ignore . entry */
      if (fno.fname[0] == '.' && fno.fname[1] == '.' && fno.fname[2] == 0) continue;             /* Ignore .. entry */
  
      // add the entry to the list
      next += mb_ftp_print_eplf_item((list + next), (maxlistsize - next), &fno);
    }
    listcount++;
  }
  if (result == MB_FTP_RESULT_OK)
  {
    mb_ftp_close_files();
  }
  *listsize = next;
  return result;
}

  
STATIC mb_ftp_result_t mb_ftp_open_dir_for_listing (const char *path) 
{
  // "hack" to detect the root directory
  if (path[0] == '/' && path[1] == '\0') 
  {
    mb_ftp_manager.listroot = true;
  } 
  else
  {
  FRESULT res=FR_OK;
  res = f_opendir_helper(&(mb_ftp_manager).dp, path);                       /* Open the directory */
  if (res != FR_OK)
  {
    return MB_FTP_RESULT_FAILED;
  }
  mb_ftp_manager.e_open = MB_FTP_DIR_OPEN;
  mb_ftp_manager.listroot = false;
  }
  return MB_FTP_RESULT_CONTINUE;
}


void mb_ftp_init()
{
  // Allocate memory for the data buffer, and the file system structs (from the RTOS heap)
  mb_ftp_manager.ftpBuffer = pvPortMalloc(MB_FTP_BUFFER_SIZE);
  mb_ftp_path = pvPortMalloc(MB_FTP_MAX_PARAM_SIZE);
  mb_ftp_scratch_buffer = pvPortMalloc(MB_FTP_MAX_PARAM_SIZE);
  mb_ftp_cmd_buffer = pvPortMalloc(MB_FTP_MAX_PARAM_SIZE + MB_FTP_CMD_SIZE_MAX);
  SOCKETFIFO_Init (&mb_ftp_socketfifo, (void *)mb_ftp_fifoelements, MB_FTP_SOCKETFIFO_ELEMENTS_MAX);
  mb_ftp_manager.cc_sd  = -1;
  mb_ftp_manager.cd_sd  = -1;
  mb_ftp_manager.sc_sd = -1;
  mb_ftp_manager.sd_sd = -1;
  mb_ftp_manager.e_open = MB_FTP_NOTHING_OPEN;
  mb_ftp_manager.state = MB_FTP_DISABLED;
  mb_ftp_manager.data_trans_state = MB_FTP_DATATRANS_DISCONNECTED;
  mb_ftp_manager.enabled=false;
  mb_ftp_manager.special_file = false;
  mb_ftp_manager.listroot=false;
  mb_ftp_manager.volcount = 0;
  mb_ftp_manager.loggin.passvalid=false;
  mb_ftp_manager.loggin.uservalid=false;
  mb_ftp_manager.loggin.userloggined=false;

}


void mb_ftp_run()
{
  switch(mb_ftp_manager.state)
  {
    case MB_FTP_DISABLED:
		 mb_ftp_enabled_check();  
		 break;
    case MB_FTP_CONTROL_START:
	     if(mb_ftp_create_controlanddata_socket(&mb_ftp_manager.sc_sd,MB_FTP_CMD_PORT))
	     {
           mb_ftp_manager.state=MB_FTP_CONTROL_CONNECTED;
		 }
		 break;

    case MB_FTP_CONTROL_CONNECTED:
  	     if (mb_ftp_manager.cc_sd < 0 &&  mb_ftp_manager.data_trans_state == MB_FTP_DATATRANS_DISCONNECTED) 
		 {
           if (MB_FTP_RESULT_OK == mb_ftp_wait_for_connection(mb_ftp_manager.sc_sd, &mb_ftp_manager.cc_sd, &mb_ftp_manager.ip_addr)) 
 		  {
             mb_ftp_manager.txRetries = 0;
             mb_ftp_manager.logginRetries = 0;
             mb_ftp_manager.ctimeout = 0;
             mb_ftp_manager.loggin.uservalid = false;
             mb_ftp_manager.loggin.passvalid = false;
			 mb_ftp_manager.loggin.userloggined=false;
             strcpy (mb_ftp_path, "/");
             mb_ftp_send_reply (220, "Makeblock:Micropython  FTP Server");
           }
         }
		 else
		 {
		   mb_ftp_cmd_process();
		 }
		 break;	 
    case MB_FTP_STE_CONTINUE_LISTING:
         // go on with listing only if the transmit buffer is empty
         if (SOCKETFIFO_IsEmpty())
		 {
		   uint32_t listsize=0;
           mb_ftp_list_dir((char *)mb_ftp_manager.ftpBuffer, MB_FTP_BUFFER_SIZE, &listsize);
           if (listsize > 0)
		   {
			 mb_ftp_send_data_to_fifo(listsize);
           } 
		   else
		   {
             mb_ftp_send_reply(226, NULL);
             mb_ftp_manager.state = MB_FTP_STE_END_TRANSFER;
           }
           mb_ftp_manager.ctimeout = 0;
         }
	 
		 break;
    case MB_FTP_STE_CONTINUE_FILE_TX:
         // read the next block from the file only if the previous one has been sent
         if (SOCKETFIFO_IsEmpty())
		 {
           uint32_t readsize;
           mb_ftp_result_t result;
           mb_ftp_manager.ctimeout = 0;
           result = mb_ftp_read_file ((char *)mb_ftp_manager.ftpBuffer, MB_FTP_BUFFER_SIZE, &readsize);
           if (result == MB_FTP_RESULT_FAILED)
		   {
             mb_ftp_send_reply(451, NULL);
             mb_ftp_manager.state = MB_FTP_STE_END_TRANSFER;
           }
           else
		   {
             if (readsize > 0)
			 {
               mb_ftp_send_data_to_fifo(readsize);
             }
             if (result == MB_FTP_RESULT_OK)
			 {
               mb_ftp_send_reply(226, NULL);
               mb_ftp_manager.state = MB_FTP_STE_END_TRANSFER;
             }
           }
         }	
		 break;
    case MB_FTP_STE_CONTINUE_FILE_RX:
		 if (SOCKETFIFO_IsEmpty())
		 {
		   int32_t len;
		   mb_ftp_result_t result;
		   if (MB_FTP_RESULT_OK == (result = mb_ftp_receive_data(mb_ftp_manager.cd_sd, mb_ftp_manager.ftpBuffer, MB_FTP_BUFFER_SIZE, &len)))
		   {
			 mb_ftp_manager.dtimeout = 0;
			 mb_ftp_manager.ctimeout = 0;
			 // its a software update
			 if (mb_ftp_manager.special_file)
			 {
		       if (1/*updater_write(ftp_data.dBuffer, len)*/)
			   {
				 break;
			   }
		     }
		     // user file being received
			 else if (MB_FTP_RESULT_OK == mb_ftp_write_file ((char *)mb_ftp_manager.ftpBuffer, len)) 
			 {
			  break;
			 }
			 mb_ftp_send_reply(451, NULL);
			 mb_ftp_manager.state = MB_FTP_STE_END_TRANSFER;
		   }
		   else if (result == MB_FTP_RESULT_CONTINUE)
		   {
			 if (mb_ftp_manager.dtimeout++ > 10000000) 
			 {
			   mb_ftp_close_files();
			   mb_ftp_send_reply(426, NULL);
			   mb_ftp_manager.state = MB_FTP_STE_END_TRANSFER;
			 }
		   }
		   else
		   {
			 if (mb_ftp_manager.special_file) 
			 {
				mb_ftp_manager.special_file = false;
				//updater_finnish();
			 }
			 mb_ftp_close_files();
			 mb_ftp_send_reply(226, NULL);
			 mb_ftp_manager.state = MB_FTP_STE_END_TRANSFER;
		   }
		 }

		 break;
	case MB_FTP_STE_END_TRANSFER:
		 break;	 
	default:
		 break;

  }
  /*process the data transmit channel*/
  if(mb_ftp_manager.loggin.userloggined)//fftust:only do it after the client has loggined
  {
    switch(mb_ftp_manager.data_trans_state)
    {
      case MB_FTP_DATATRANS_DISCONNECTED:
	  	   break;
	  case MB_FTP_DATATRANS_LISTEN_FOR_DATA:
           if (MB_FTP_RESULT_OK == mb_ftp_wait_for_connection(mb_ftp_manager.sd_sd, &mb_ftp_manager.cd_sd, NULL))
		   {
             mb_ftp_manager.dtimeout = 0;
             mb_ftp_manager.data_trans_state = MB_FTP_DATATRANS_CONNECTED;
           } 
		   else if (mb_ftp_manager.dtimeout++ > 100000000/*FTP_DATA_TIMEOUT_MS / FTP_CYCLE_TIME_MS*/) 
		   {
             mb_ftp_manager.dtimeout = 0;
             /* close the listening socket*/
             mb_ftp_close_socket(&mb_ftp_manager.sd_sd);
             mb_ftp_manager.data_trans_state= MB_FTP_DATATRANS_DISCONNECTED;
           }
		   break;
      case MB_FTP_DATATRANS_CONNECTED:
           if (mb_ftp_manager.state == MB_FTP_CONTROL_CONNECTED && mb_ftp_manager.dtimeout++ > 100000000/*FTP_DATA_TIMEOUT_MS / FTP_CYCLE_TIME_MS*/)
		   {
             mb_ftp_manager.dtimeout=0;
 			 // close the listening and the data socket
             mb_ftp_close_socket(&mb_ftp_manager.sd_sd);
             mb_ftp_close_socket(&mb_ftp_manager.cd_sd);
             mb_ftp_close_filesystem_on_error ();
             mb_ftp_manager.data_trans_state = MB_FTP_DATATRANS_DISCONNECTED;
           }
		   break;

	}


  }

  
  mb_ftp_send_from_fifo();//fftust:send the data  that haved writed to the fifo 
  
  if (mb_ftp_manager.cd_sd < 0 && (mb_ftp_manager.state > MB_FTP_CONTROL_CONNECTED)) //fftust:if data disconnected, stop the data transmit 
  {
    mb_ftp_manager.data_trans_state =MB_FTP_DATATRANS_DISCONNECTED;
    mb_ftp_manager.state = MB_FTP_CONTROL_CONNECTED;
  }
}

STATIC void mb_ftp_cmd_process()
{
 
  int32_t receivedata_len;
  char *bufptr = (char *)mb_ftp_cmd_buffer;
  mb_ftp_result_t result;
  FRESULT fres;
  FILINFO fno;
  
  mb_ftp_manager.closechild = false;
  if (MB_FTP_RESULT_OK == (result = mb_ftp_receive_data(mb_ftp_manager.cc_sd, mb_ftp_cmd_buffer, MB_FTP_MAX_PARAM_SIZE + MB_FTP_CMD_SIZE_MAX, &receivedata_len)))
  {
    
	mb_ftp_cmd_index_t cmd = mb_ftp_pop_command(&bufptr);
	//mb_ftp_send_data_non_blocking(mb_ftp_manager.cc_sd,&receivedata_len,1); //fftust: debug
	switch(cmd)
	{
      case MB_FTP_CMD_PASV:
      {
        mb_ftp_close_socket(&mb_ftp_manager.cd_sd);
        mb_ftp_manager.data_trans_state = MB_FTP_DATATRANS_DISCONNECTED;
        bool socketcreated = true;
        if (mb_ftp_manager.sd_sd < 0) 
		{
          socketcreated = mb_ftp_create_controlanddata_socket(&mb_ftp_manager.sd_sd, MB_FTP_PASIVE_DATA_PORT);
        }
        if (socketcreated)
		{
          uint8_t *pip = (uint8_t *)&mb_ftp_manager.ip_addr;
          mb_ftp_manager.dtimeout = 0;
          snprintf((char *)mb_ftp_manager.ftpBuffer, MB_FTP_BUFFER_SIZE, "(%u,%u,%u,%u,%u,%u)", pip[0], pip[1], pip[2], pip[3], (MB_FTP_PASIVE_DATA_PORT >> 8), (MB_FTP_PASIVE_DATA_PORT & 0xFF));
          mb_ftp_manager.data_trans_state = MB_FTP_DATATRANS_LISTEN_FOR_DATA; //fftust:mb_ftp_run() will deal the later process 
          mb_ftp_send_reply(227, (char *)mb_ftp_manager.ftpBuffer);//fftust:access the pasive mode
        } 
		else
		{
          mb_ftp_send_reply(425, NULL);//fftust:cann't open the data connector
        }
      }
      break;
	  case MB_FTP_CMD_USER:   
           mb_ftp_pop_param (&bufptr, mb_ftp_scratch_buffer);
		   //mb_ftp_send_data_non_blocking(mb_ftp_manager.cc_sd,mb_ftp_scratch_buffer,strlen(mb_ftp_scratch_buffer));  //fftust: debug
           if (!memcmp(mb_ftp_scratch_buffer, mb_ftp_user, MAX(strlen(mb_ftp_scratch_buffer), strlen(mb_ftp_user))))
		   {
             mb_ftp_manager.loggin.uservalid = true && (strlen(mb_ftp_user) == strlen(mb_ftp_scratch_buffer));
			 if(mb_ftp_manager.loggin.uservalid)
			 {
               mb_ftp_send_reply(331, NULL);
			 }  
           }
           
		   break;
	  case MB_FTP_CMD_PASS:
           mb_ftp_pop_param (&bufptr, mb_ftp_scratch_buffer);
           if (!memcmp(mb_ftp_scratch_buffer, mb_ftp_pass, MAX(strlen(mb_ftp_scratch_buffer), strlen(mb_ftp_pass))) && mb_ftp_manager.loggin.uservalid)
		   {
             mb_ftp_manager.loggin.passvalid = true && (strlen(mb_ftp_pass) == strlen(mb_ftp_scratch_buffer));
             if (mb_ftp_manager.loggin.passvalid)
			 {
               mb_ftp_manager.loggin.userloggined=true; //fftust:client loggined,client can open a data connector to read/write data
			   mb_ftp_send_reply(230, NULL);
               break;
             }
           }
           mb_ftp_send_reply(530, NULL);
		   break;
      case MB_FTP_CMD_CWD:
           fres = FR_NO_PATH;
           mb_ftp_pop_param (&bufptr, mb_ftp_scratch_buffer);
           mb_ftp_open_child (mb_ftp_path, mb_ftp_scratch_buffer);
           if ((mb_ftp_path[0] == '/' && mb_ftp_path[1] == '\0') || ((fres = f_opendir_helper (&mb_ftp_manager.dp, mb_ftp_path)) == FR_OK))
   		   {
   			if (fres == FR_OK)
   			{
              f_closedir(&mb_ftp_manager.dp);	  
            }
             mb_ftp_send_reply(250, NULL);
           } 
   		   else
   		   {
             mb_ftp_close_child (mb_ftp_path);
             mb_ftp_send_reply(550, NULL);
           }
           break;
     case MB_FTP_CMD_LIST:
	 	  if (mb_ftp_open_dir_for_listing(mb_ftp_path) == MB_FTP_RESULT_CONTINUE)
		  {
			mb_ftp_manager.state = MB_FTP_STE_CONTINUE_LISTING;
			mb_ftp_send_reply(150, NULL);
          } 
		  else
		  {
            mb_ftp_send_reply(550, NULL);
          }
          break;  
	 case MB_FTP_CMD_RETR:
		  mb_ftp_get_param_and_open_child (&bufptr);
		  if (mb_ftp_open_file (mb_ftp_path, FA_READ)) 
		  {
		    mb_ftp_manager.state = MB_FTP_STE_CONTINUE_FILE_TX;
		    mb_ftp_send_reply(150, NULL);
		  }
		  else
		  {
	        mb_ftp_manager.state = MB_FTP_STE_END_TRANSFER;
		    mb_ftp_send_reply(550, NULL);
		  }
		  break;
     case MB_FTP_CMD_STOR:
     	  mb_ftp_get_param_and_open_child (&bufptr);
          if (mb_ftp_open_file (mb_ftp_path, FA_WRITE | FA_CREATE_ALWAYS))
          {
        	mb_ftp_manager.state = MB_FTP_STE_CONTINUE_FILE_RX;
        	mb_ftp_send_reply(150, NULL);
          }
          else
          {
            mb_ftp_manager.state = MB_FTP_STE_END_TRANSFER;
        	mb_ftp_send_reply(550, NULL);
          }
     	  break;
  
     case MB_FTP_CMD_DELE:
     case MB_FTP_CMD_RMD:
          mb_ftp_get_param_and_open_child (&bufptr);
          if (FR_OK == f_unlink_helper(mb_ftp_path))
 		  {
            mb_ftp_send_reply(250, NULL);
          } 
 		  else
 		  {
            mb_ftp_send_reply(550, NULL);
          }
          break;
     case MB_FTP_CMD_MKD:
          mb_ftp_get_param_and_open_child (&bufptr);
          if (FR_OK == f_mkdir_helper(mb_ftp_path))
   		  {
            mb_ftp_send_reply(250, NULL);
          } 
   		  else 
   		  {
            mb_ftp_send_reply(550, NULL);
          }
          break;
     case MB_FTP_CMD_RNFR:
          mb_ftp_get_param_and_open_child (&bufptr);
          if (FR_OK == f_stat_helper(mb_ftp_path, &fno)) 
		  {
            mb_ftp_send_reply(350, NULL);
            // save the current path
            strcpy ((char *)mb_ftp_manager.ftpBuffer, mb_ftp_path);
          } 
		  else
		  {
            mb_ftp_send_reply(550, NULL);
          }
          break;
     case MB_FTP_CMD_RNTO:
          mb_ftp_get_param_and_open_child (&bufptr);
          // old path was saved in the data buffer
          if (FR_OK == (fres = f_rename_helper((char *)mb_ftp_manager.ftpBuffer, mb_ftp_path)))
		  {
            mb_ftp_send_reply(250, NULL);
          } 
		  else
		  {
            mb_ftp_send_reply(550, NULL);
          }
          break;
     case MB_FTP_CMD_SIZE:
          {
            mb_ftp_get_param_and_open_child (&bufptr);
            if (FR_OK == f_stat_helper(mb_ftp_path, &fno)) 
			{
              // send the size
              snprintf((char *)mb_ftp_manager.ftpBuffer, MB_FTP_BUFFER_SIZE, "%u", (uint32_t)fno.fsize);
              mb_ftp_send_reply(213, (char *)mb_ftp_manager.ftpBuffer);
            }
            else
			{
              mb_ftp_send_reply(550, NULL);
            }
          }
          break;
      case MB_FTP_CMD_MDTM:
           mb_ftp_get_param_and_open_child (&bufptr);
           if (FR_OK == f_stat_helper(mb_ftp_path, &fno)) 
 		   {
             // send the last modified time
             snprintf((char *)mb_ftp_manager.ftpBuffer, MB_FTP_BUFFER_SIZE, "%u%02u%02u%02u%02u%02u",
                       1980 + ((fno.fdate >> 9) & 0x7f), (fno.fdate >> 5) & 0x0f,
                       fno.fdate & 0x1f, (fno.ftime >> 11) & 0x1f,
                       (fno.ftime >> 5) & 0x3f, 2 * (fno.ftime & 0x1f));
             mb_ftp_send_reply(213, (char *)mb_ftp_manager.ftpBuffer);
           }
           else
 		   {
             mb_ftp_send_reply(550, NULL);
           }
           break;		  
      case MB_FTP_CMD_CDUP:
           mb_ftp_close_child(mb_ftp_path);
           mb_ftp_send_reply(250, NULL);
            break;
      case MB_FTP_CMD_PWD:

      case MB_FTP_CMD_XPWD:
           mb_ftp_send_reply(257, mb_ftp_path);
           break;
		   
	  case MB_FTP_CMD_TYPE:
           mb_ftp_send_reply(200, NULL);
           break;   
	  case MB_FTP_CMD_FEAT:
           mb_ftp_send_reply(211, "no-features");
           break;
      case MB_FTP_CMD_SYST:
           mb_ftp_send_reply(215, "UNIX Type: L8");
           break;
	  case MB_FTP_CMD_NOOP:
           mb_ftp_send_reply(200, NULL);	   
      case MB_FTP_CMD_QUIT:
	 	   mb_ftp_send_reply(221, NULL);
           break;
      default:
	  	   mb_ftp_send_reply(502,"the command is unknow");
	  	   break;

    }

	if (mb_ftp_manager.closechild)
	{
      mb_ftp_return_to_previous_path(mb_ftp_path, mb_ftp_scratch_buffer);
    }
    
  }
  else if (result == MB_FTP_RESULT_CONTINUE) 
  {
  	if (mb_ftp_manager.ctimeout++ > 100000000/*(servers_get_timeout() / FTP_CYCLE_TIME_MS)*/) 
	{
	  mb_ftp_manager.ctimeout=0;
  	}
  } 
  else
  {
	mb_ftp_close_client_sockets();
  }

}


