#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "wm_include.h"
#include "sockets.h"
#include "netif.h"

#include "extmod/vfs_fat.h"
#include "lib/oofatfs/ff.h"
#include "mpthreadport.h"

//#define FTPS_DBG printf
#define FTPS_DBG(...)

#define FTP_SRV_ROOT        "/"
#define FTP_MAX_CONNECTION  2
#define FTP_WELCOME_MSG     "220-= welcome on W600 FTP server =-\r\n220 \r\n"
#define FTP_BUFFER_SIZE     512

struct ftp_session {
    bool is_anonymous;

    int sockfd;
    struct sockaddr_in remote;

    /* pasv data */
    char pasv_active;
    int  pasv_sockfd;

    unsigned short pasv_port;
    size_t offset;

    /* current directory */
    char currentdir[256];
    char rename[256];

    struct netif *netif;
    int pasv_acpt_sockfd;

    struct ftp_session *next;
};
static struct ftp_session *session_list = NULL;

#define MPY_FTPS_SIZE   512
static OS_STK mpy_ftps_stk[MPY_FTPS_SIZE];

static int is_run = 0;
static int ftpsport = 21;
static char username[32] = {0};
static char userpwd[64] = {0};

#if MICROPY_USE_INTERVAL_FLS_FS
extern fs_user_mount_t *spi_fls_vfs_fat;
#endif

int ftp_process_request(struct ftp_session *session, char *buf);
int ftp_get_filesize(char *filename);

struct ftp_session *ftp_new_session(void) {
    struct ftp_session *session;

    session = (struct ftp_session *)tls_mem_alloc(sizeof(struct ftp_session));

    session->sockfd = -1;
    session->pasv_sockfd = -1;
    session->pasv_acpt_sockfd = -1;

    session->next = session_list;
    session_list = session;

    return session;
}

void ftp_close_session(struct ftp_session *session) {
    struct ftp_session *list;

    if (session_list == session) {
        session_list = session_list->next;
        session->next = NULL;
    } else {
        list = session_list;
        while (list->next != session) list = list->next;

        list->next = session->next;
        session->next = NULL;
    }

    tls_mem_free(session);
}

int ftp_get_filesize(char *filename) {
    fs_user_mount_t *vfs_fat = spi_fls_vfs_fat;
    FILINFO fno;
    FRESULT res = f_stat (&vfs_fat->fatfs, filename, &fno);
    if (FR_OK != res) return -1;
    return fno.fsize;
}

bool is_absolute_path(char *path) {
#ifdef _WIN32
    if (path[0] == '\\' ||
            (path[1] == ':' && path[2] == '\\'))
        return TRUE;
#else
    if (path[0] == '/') return TRUE;
#endif

    return FALSE;
}

int build_full_path(struct ftp_session *session, char *path, char *new_path, size_t size) {
    if (is_absolute_path(path) == TRUE) {
        strcpy(new_path, path);
    } else {
        sprintf(new_path, "%s/%s", session->currentdir, path);
    }

    if ((strlen(new_path) > 2) && new_path[strlen(new_path) - 1] == '/')
        new_path[strlen(new_path) - 1] = '\0';

    return 0;
}

static void w600_ftps_task(void *param) {
    int numbytes;
    struct timeval tv;
    int sockfd, maxfdp1;
    struct sockaddr_in local;
    fd_set readfds, tmpfds;
    struct ftp_session *session;
    int ret;
    u32 addr_len = sizeof(struct sockaddr);
    char *buffer = (char *) tls_mem_alloc(FTP_BUFFER_SIZE);

    local.sin_port = htons(ftpsport);
    local.sin_family = PF_INET;
    local.sin_addr.s_addr = INADDR_ANY;

    FD_ZERO(&readfds);
    FD_ZERO(&tmpfds);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        FTPS_DBG("create socket failed\n");
        return ;
    }

    ret  = bind(sockfd, (struct sockaddr *)&local, addr_len);
    ret |= listen(sockfd, FTP_MAX_CONNECTION);
    if (ret) {
        closesocket(sockfd);
        tls_mem_free(buffer);
        return;
    }

    printf("ftpserver is running.\r\n");
    FD_SET(sockfd, &readfds);
    tv.tv_sec  = 0;
    tv.tv_usec = 100 * 1000;
    for(;;) {
        /* get maximum fd */
        maxfdp1 = sockfd + 1;
        session = session_list;
        while (session != NULL) {
            if (maxfdp1 < session->sockfd + 1)
                maxfdp1 = session->sockfd + 1;

            FD_SET(session->sockfd, &readfds);
            session = session->next;
        }

        tmpfds = readfds;
        if (select(maxfdp1, &tmpfds, 0, 0, &tv) == 0) continue;

        if (FD_ISSET(sockfd, &tmpfds)) {
            int com_socket;
            struct sockaddr_in remote;

            com_socket = accept(sockfd, (struct sockaddr *)&remote, &addr_len);
            if (com_socket == -1) {
                FTPS_DBG("Error on accept()\nContinuing...\n");
                tls_os_time_delay(2);
                continue;
            } else {
                FTPS_DBG("Got connection from %s\n", inet_ntoa(remote.sin_addr));
                send(com_socket, FTP_WELCOME_MSG, strlen(FTP_WELCOME_MSG), 0);
                FD_SET(com_socket, &readfds);

                /* new session */
                session = ftp_new_session();
                if (session != NULL) {
                    fs_user_mount_t *vfs_fat = spi_fls_vfs_fat;
                    f_chdir (&vfs_fat->fatfs, "/");
                    strcpy(session->currentdir, FTP_SRV_ROOT);
                    session->sockfd = com_socket;
                    session->remote = remote;

                    struct netif *netif = tls_get_netif();
                    if ((netif->ip_addr.addr & 0xFFFFFF) == (remote.sin_addr.s_addr & 0xFFFFFF)) {
                        session->netif = netif;
                    } else {
                        session->netif = netif->next;
                    }
                }
            }
        }

        {
            struct ftp_session *next;

            session = session_list;
            while (session != NULL) {
                next = session->next;
                if (FD_ISSET(session->sockfd, &tmpfds)) {
                    numbytes = recv(session->sockfd, buffer, FTP_BUFFER_SIZE, 0);
                    if (numbytes == 0 || numbytes == -1) {
                        FTPS_DBG("Client %s disconnected %d, %d\n", inet_ntoa(session->remote.sin_addr), __LINE__, session->sockfd);
                        FD_CLR(session->sockfd, &readfds);
                        closesocket(session->sockfd);
                        ftp_close_session(session);
                    } else {
                        buffer[numbytes] = 0;
                        if (ftp_process_request(session, buffer) == -1) {
                            FTPS_DBG("Client %s disconnected %d, %d\r\n", inet_ntoa(session->remote.sin_addr), __LINE__, session->sockfd);
                            FD_CLR(session->sockfd, &readfds);
                            closesocket(session->sockfd);
                            ftp_close_session(session);
                        }
                    }
                }

                session = next;
            }
        }
    }

#if 0
    struct ftp_session *next;
    session = session_list;
    while (session != NULL) {
        next = session->next;
        if (-1 != session->sockfd)
            closesocket(session->sockfd);
        if (-1 != session->pasv_sockfd)
            closesocket(session->pasv_sockfd);
        tls_mem_free(session);
        session = next;
    }
    closesocket(sockfd);
    tls_mem_free(buffer);
#endif
}

void w600_ftps_start(int port, char *user, char *pass) {
    ftpsport = port;
    strcpy(username, user);
    strcpy(userpwd, pass);

    if (is_run)
        return;

    tls_os_task_create(NULL, "w600_ftps_task", w600_ftps_task, NULL,
                       (void *)mpy_ftps_stk, MPY_FTPS_SIZE * sizeof(OS_STK),
                       MPY_FTPS_PRIO, 0);
    is_run = 1;
}

int do_list(char *directory, int sockfd) {
    char line_buffer[256], line_length;
#ifdef _WIN32
    //struct _stat s;
#else
    //struct stat s;
#endif

    fs_user_mount_t *vfs_fat = spi_fls_vfs_fat;
    FF_DIR dir;
    FRESULT res = f_opendir (&vfs_fat->fatfs, &dir, directory);

    if (res != FR_OK) {
        line_length = sprintf(line_buffer, "500 Internal Error\r\n");
        send(sockfd, line_buffer, line_length, 0);
        return -1;
    }

    FILINFO fno;
    while (1) {
        res = f_readdir (&dir, &fno);
        if ((res != FR_OK) || (fno.fname[0] == 0))
            break;

        //sprintf(line_buffer, "%s/%s", directory, fno.fname);
#ifdef _WIN32
        //if (_stat(line_buffer, &s) ==0)
#else
        //if (stat(line_buffer, &s) == 0)
#endif
        //{
        line_length = sprintf(line_buffer, "%srwxrwxrwx %3d root root %6d Jan 1 2018 %s\r\n", (fno.fattrib & AM_DIR) ? "d" : "-", 0, fno.fsize, fno.fname);

        send(sockfd, line_buffer, line_length, 0);
        //}
        //else
        //{
        //  FTPS_DBG("Get directory entry error\n");
        //  break;
        //}
    }

    f_closedir(&dir);
    return 0;
}

int do_simple_list(char *directory, int sockfd) {
    char line_buffer[256], line_length;

    fs_user_mount_t *vfs_fat = spi_fls_vfs_fat;
    FF_DIR dir;
    FRESULT res = f_opendir (&vfs_fat->fatfs, &dir, directory);

    if (res != FR_OK) {
        line_length = sprintf(line_buffer, "500 Internal Error\r\n");
        send(sockfd, line_buffer, line_length, 0);
        return -1;
    }

    FILINFO fno;
    while (1) {
        res = f_readdir (&dir, &fno);
        if ((res != FR_OK) || (fno.fname[0] == 0))
            break;

        line_length = sprintf(line_buffer, "%s\r\n", fno.fname);
        send(sockfd, line_buffer, line_length, 0);
    }

    f_closedir(&dir);
    return 0;
}

int str_begin_with(char *src, char *match) {
    while (*match) {
        /* check source */
        if (*src == 0) return -1;

        if (*match != *src) return -1;
        match ++;
        src ++;
    }

    return 0;
}

int ftp_get_pasv_sock(struct ftp_session *session) {
#if 0
    struct timeval tv;
    fd_set readfds;
    char *sbuf;
    u32 addr_len = sizeof(struct sockaddr_in);
    struct sockaddr_in local, pasvremote;

    if (!session->pasv_active)
        return 0;

    sbuf = (char *)tls_mem_alloc(FTP_BUFFER_SIZE);

    tv.tv_sec = 3, tv.tv_usec = 0;
    FD_ZERO(&readfds);
    FD_SET(session->pasv_acpt_sockfd, &readfds);
    FTPS_DBG("Listening %d seconds @ port %d\n", tv.tv_sec, session->pasv_port);
    select(session->pasv_acpt_sockfd + 1, &readfds, 0, 0, &tv);
    if (FD_ISSET(session->pasv_acpt_sockfd, &readfds)) {
        if ((session->pasv_sockfd = accept(session->pasv_acpt_sockfd, (struct sockaddr *)&pasvremote, &addr_len)) == -1) {
            sprintf(sbuf, "425 Can't open data connection %d.\r\n", __LINE__);
            send(session->sockfd, sbuf, strlen(sbuf), 0);
            goto err1;
        } else {
            FTPS_DBG("Got Data(PASV) connection from %s\n", inet_ntoa(pasvremote.sin_addr));
            session->pasv_active = 1;
            closesocket(session->pasv_acpt_sockfd);
            session->pasv_acpt_sockfd = -1;
        }
    } else {
err1:
        if (-1 != session->pasv_acpt_sockfd) {
            closesocket(session->pasv_acpt_sockfd);
            session->pasv_acpt_sockfd = -1;
        }
        if (-1 != session->pasv_sockfd) {
            closesocket(session->pasv_sockfd);
            session->pasv_sockfd = -1;
        }
        session->pasv_active = 0;

    }
    tls_mem_free(sbuf);
#endif
    return 0;
}

int ftp_process_request(struct ftp_session *session, char *buf) {
    struct timeval tv;
    fd_set readfds;
    char filename[256];
    int  numbytes;
    char *sbuf;
    char *parameter_ptr, *ptr;
    u32 addr_len = sizeof(struct sockaddr_in);
    struct sockaddr_in local, pasvremote;

    sbuf = (char *)tls_mem_alloc(FTP_BUFFER_SIZE);

    tv.tv_sec = 3, tv.tv_usec = 0;
    local.sin_family = PF_INET;
    local.sin_addr.s_addr = INADDR_ANY;

    /* remove \r\n */
    ptr = buf;
    while (*ptr) {
        if (*ptr == '\r' || *ptr == '\n') *ptr = 0;
        ptr ++;
    }

    /* get request parameter */
    parameter_ptr = strchr(buf, ' ');
    if (parameter_ptr != NULL) parameter_ptr ++;

    // debug:
    FTPS_DBG("%s requested: \"%s\"\n", inet_ntoa(session->remote.sin_addr), buf);

    //
    //-----------------------
    if (str_begin_with(buf, "USER") == 0) {
        FTPS_DBG("%s sent login \"%s\"\n", inet_ntoa(session->remote.sin_addr), parameter_ptr);
        // login correct
        if (strcmp(parameter_ptr, "anonymous") == 0) {
            session->is_anonymous = TRUE;
            sprintf(sbuf, "331 Anonymous login OK send e-mail address for password.\r\n", parameter_ptr);
            send(session->sockfd, sbuf, strlen(sbuf), 0);
        } else if (strcmp(parameter_ptr, username) == 0) {
            session->is_anonymous = FALSE;
            sprintf(sbuf, "331 Password required for %s\r\n", parameter_ptr);
            send(session->sockfd, sbuf, strlen(sbuf), 0);
        } else {
            // incorrect login
            sprintf(sbuf, "530 Login incorrect. Bye.\r\n");
            send(session->sockfd, sbuf, strlen(sbuf), 0);
            tls_mem_free(sbuf);
            return -1;
        }
        return 0;
    } else if (str_begin_with(buf, "PASS") == 0) {
        FTPS_DBG("%s sent password \"%s\"\n", inet_ntoa(session->remote.sin_addr), parameter_ptr);
        if (strcmp(parameter_ptr, userpwd) == 0 || session->is_anonymous == TRUE) {
            // password correct
            sprintf(sbuf, "230 User logged in\r\n");
            send(session->sockfd, sbuf, strlen(sbuf), 0);
            tls_mem_free(sbuf);
            //session->is_anonymous == FALSE;
            return 0;
        }

        // incorrect password
        sprintf(sbuf, "530 Login or Password incorrect. Bye!\r\n");
        send(session->sockfd, sbuf, strlen(sbuf), 0);
        tls_mem_free(sbuf);
        return -1;
    } else if (str_begin_with(buf, "LIST") == 0  ) {
        memset(sbuf, 0, FTP_BUFFER_SIZE);
        sprintf(sbuf, "150 Opening Binary mode connection for file list.\r\n");
        send(session->sockfd, sbuf, strlen(sbuf), 0);
        ftp_get_pasv_sock(session);
        do_list(session->currentdir, session->pasv_sockfd);
        closesocket(session->pasv_sockfd);
        session->pasv_sockfd = -1;
        session->pasv_active = 0;
        sprintf(sbuf, "226 Transfert Complete.\r\n");
        send(session->sockfd, sbuf, strlen(sbuf), 0);
    } else if (str_begin_with(buf, "NLST") == 0 ) {
        memset(sbuf, 0, FTP_BUFFER_SIZE);
        sprintf(sbuf, "150 Opening Binary mode connection for file list.\r\n");
        send(session->sockfd, sbuf, strlen(sbuf), 0);
        ftp_get_pasv_sock(session);
        do_simple_list(session->currentdir, session->pasv_sockfd);
        closesocket(session->pasv_sockfd);
        session->pasv_sockfd = -1;
        session->pasv_active = 0;
        sprintf(sbuf, "226 Transfert Complete.\r\n");
        send(session->sockfd, sbuf, strlen(sbuf), 0);
    } else if (str_begin_with(buf, "PWD") == 0 || str_begin_with(buf, "XPWD") == 0) {
        sprintf(sbuf, "257 \"%s\" is current directory.\r\n", session->currentdir);
        send(session->sockfd, sbuf, strlen(sbuf), 0);
    } else if (str_begin_with(buf, "TYPE") == 0) {
        // Ignore it
        if (strcmp(parameter_ptr, "I") == 0) {
            sprintf(sbuf, "200 Type set to binary.\r\n");
            send(session->sockfd, sbuf, strlen(sbuf), 0);
        } else {
            sprintf(sbuf, "200 Type set to ascii.\r\n");
            send(session->sockfd, sbuf, strlen(sbuf), 0);
        }
    } else if (str_begin_with(buf, "PASV") == 0) {
        int dig1, dig2;
        int sockfd;
        char optval = '1';

        session->pasv_port = 10000;
        session->pasv_active = 1;
        local.sin_port = htons(session->pasv_port);
        local.sin_addr.s_addr = INADDR_ANY;

        dig1 = (int)(session->pasv_port / 256);
        dig2 = session->pasv_port % 256;

        if (-1 != session->pasv_sockfd) {
            closesocket(session->pasv_sockfd);

            session->pasv_sockfd = -1;
        }
        if ((sockfd = socket(PF_INET, SOCK_STREAM, 0)) == -1) {
            sprintf(sbuf, "425 Can't open data connection %d.\r\n", __LINE__);
            send(session->sockfd, sbuf, strlen(sbuf), 0);
            goto err1;
        }
#if 1
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
            sprintf(sbuf, "425 Can't open data connection %d.\r\n", __LINE__);
            send(session->sockfd, sbuf, strlen(sbuf), 0);
            goto err1;
        }
#endif
        if (bind(sockfd, (struct sockaddr *)&local, addr_len) == -1) {
            sprintf(sbuf, "425 Can't open data connection %d.\r\n", __LINE__);
            send(session->sockfd, sbuf, strlen(sbuf), 0);
            goto err1;
        }
        if (listen(sockfd, 1) == -1) {
            sprintf(sbuf, "425 Can't open data connection %d.\r\n", __LINE__);
            send(session->sockfd, sbuf, strlen(sbuf), 0);
            goto err1;
        }
        sprintf(sbuf, "227 Entering passive mode (%d,%d,%d,%d,%d,%d)\r\n", ip4_addr1(ip_2_ip4(&session->netif->ip_addr)),
                ip4_addr2(ip_2_ip4(&session->netif->ip_addr)),
                ip4_addr3(ip_2_ip4(&session->netif->ip_addr)),
                ip4_addr4(ip_2_ip4(&session->netif->ip_addr)),
                dig1, dig2);
        send(session->sockfd, sbuf, strlen(sbuf), 0);
        if (-1 != session->pasv_acpt_sockfd)
            closesocket(session->pasv_acpt_sockfd);
        session->pasv_acpt_sockfd = sockfd;

#if 0
        tls_mem_free(sbuf);
        return 0;
#else
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        FTPS_DBG("Listening %d seconds @ port %d\n", tv.tv_sec, session->pasv_port);
        select(sockfd + 1, &readfds, 0, 0, &tv);
        if (FD_ISSET(sockfd, &readfds)) {
            if ((session->pasv_sockfd = accept(sockfd, (struct sockaddr *)&pasvremote, &addr_len)) == -1) {
                sprintf(sbuf, "425 Can't open data connection %d.\r\n", __LINE__);
                send(session->sockfd, sbuf, strlen(sbuf), 0);
                goto err1;
            } else {
                FTPS_DBG("Got Data(PASV) connection from %s\n", inet_ntoa(pasvremote.sin_addr));
                session->pasv_active = 1;
                closesocket(sockfd);
            }
        } else
#endif
        {
err1:
            if (-1 != sockfd)
                closesocket(sockfd);
            if (-1 != session->pasv_sockfd) {
                closesocket(session->pasv_sockfd);
                session->pasv_sockfd = -1;
            }
            session->pasv_active = 0;
            tls_mem_free(sbuf);
            return 0;
        }
    } else if (str_begin_with(buf, "RETR") == 0) {
        int file_size;

        strcpy(filename, buf + 5);

        build_full_path(session, parameter_ptr, filename, 256);
        file_size = ftp_get_filesize(filename);
        if (file_size == -1) {
            sprintf(sbuf, "550 \"%s\" : not a regular file\r\n", filename);
            send(session->sockfd, sbuf, strlen(sbuf), 0);
            session->offset = 0;
            tls_mem_free(sbuf);
            return 0;
        }

        fs_user_mount_t *vfs_fat = spi_fls_vfs_fat;
        FIL fp;
        UINT n;
        FRESULT res = f_open(&vfs_fat->fatfs, &fp, filename, FA_READ);

        if (res != FR_OK) {
            tls_mem_free(sbuf);
            return 0;
        }

        if (session->offset > 0 && session->offset < file_size) {
            f_lseek (&fp, session->offset);
            sprintf(sbuf, "150 Opening binary mode data connection for partial \"%s\" (%d/%d bytes).\r\n",
                    filename, file_size - session->offset, file_size);
        } else {
            sprintf(sbuf, "150 Opening binary mode data connection for \"%s\" (%d bytes).\r\n", filename, file_size);
        }
        send(session->sockfd, sbuf, strlen(sbuf), 0);

        while (f_read(&fp, sbuf, FTP_BUFFER_SIZE, &numbytes) == FR_OK) {
            if (numbytes == 0)
                break;
            if (send(session->pasv_sockfd, sbuf, numbytes, 0) <= 0)
                break;
        }
        sprintf(sbuf, "226 Finished.\r\n");
        send(session->sockfd, sbuf, strlen(sbuf), 0);
        f_close(&fp);
        closesocket(session->pasv_sockfd);
        session->pasv_sockfd = -1;
    } else if (str_begin_with(buf, "STOR") == 0) {
        if (session->is_anonymous == TRUE) {
            sprintf(sbuf, "550 Permission denied.\r\n");
            send(session->sockfd, sbuf, strlen(sbuf), 0);
            tls_mem_free(sbuf);
            return 0;
        }

        build_full_path(session, parameter_ptr, filename, 256);

        fs_user_mount_t *vfs_fat = spi_fls_vfs_fat;
        FIL fp;
        UINT n;
        FRESULT res = f_open(&vfs_fat->fatfs, &fp, filename, FA_WRITE | FA_CREATE_ALWAYS);
        if (res != FR_OK) {
            sprintf(sbuf, "550 Cannot open \"%s\" for writing.\r\n", filename);
            send(session->sockfd, sbuf, strlen(sbuf), 0);
            tls_mem_free(sbuf);
            return 0;
        }
        sprintf(sbuf, "150 Opening binary mode data connection for \"%s\".\r\n", filename);
        send(session->sockfd, sbuf, strlen(sbuf), 0);
        FD_ZERO(&readfds);
        FD_SET(session->pasv_sockfd, &readfds);
        FTPS_DBG("Waiting %d seconds(%d) for data...\n", tv.tv_sec, session->pasv_sockfd);
        while (select(session->pasv_sockfd + 1, &readfds, 0, 0, &tv) > 0 ) {
            if ((numbytes = recv(session->pasv_sockfd, sbuf, FTP_BUFFER_SIZE, 0)) > 0) {
                FTPS_DBG("numbytes = %d\r\n", numbytes);
                f_write(&fp, sbuf, numbytes, &n);
            } else if (numbytes == 0) {
                FTPS_DBG("numbytes = %d\r\n", numbytes);
                closesocket(session->pasv_sockfd);
                session->pasv_sockfd = -1;
                sprintf(sbuf, "226 Finished.\r\n");
                send(session->sockfd, sbuf, strlen(sbuf), 0);
                break;
            } else if (numbytes == -1) {
                FTPS_DBG("numbytes = %d\r\n", numbytes);
                f_close(&fp);
                closesocket(session->pasv_sockfd);
                session->pasv_sockfd = -1;
                tls_mem_free(sbuf);
                return -1;
            }
        }
        f_close(&fp);
        closesocket(session->pasv_sockfd);
        session->pasv_sockfd = -1;
    } else if (str_begin_with(buf, "SIZE") == 0) {
        int file_size;

        build_full_path(session, parameter_ptr, filename, 256);

        file_size = ftp_get_filesize(filename);
        if (file_size == -1) {
            sprintf(sbuf, "550 \"%s\" : not a regular file\r\n", filename);
            send(session->sockfd, sbuf, strlen(sbuf), 0);
        } else {
            sprintf(sbuf, "213 %d\r\n", file_size);
            send(session->sockfd, sbuf, strlen(sbuf), 0);
        }
    } else if (str_begin_with(buf, "MDTM") == 0) {
        sprintf(sbuf, "550 \"/\" : not a regular file\r\n");
        send(session->sockfd, sbuf, strlen(sbuf), 0);
    } else if (str_begin_with(buf, "SYST") == 0) {
        sprintf(sbuf, "215 %s\r\n", "UNIX system type: W600 FreeRTOS");
        send(session->sockfd, sbuf, strlen(sbuf), 0);
    } else if (str_begin_with(buf, "CWD") == 0) {
        build_full_path(session, parameter_ptr, filename, 256);

        fs_user_mount_t *vfs_fat = spi_fls_vfs_fat;
        FRESULT res = f_chdir (&vfs_fat->fatfs, filename);
        if (FR_OK != res) {
            sprintf(sbuf, "550 \"%s\" : No such file or directory.\r\n", filename);
        } else {
            sprintf(sbuf, "250 Changed to directory \"%s\"\r\n", filename);
            strcpy(session->currentdir, filename);
        }
        send(session->sockfd, sbuf, strlen(sbuf), 0);
        FTPS_DBG("Changed to directory %s", filename);
    } else if (str_begin_with(buf, "CDUP") == 0) {
        sprintf(filename, "%s/%s", session->currentdir, "..");

        sprintf(sbuf, "250 Changed to directory \"%s\"\r\n", filename);
        send(session->sockfd, sbuf, strlen(sbuf), 0);
        strcpy(session->currentdir, filename);
        FTPS_DBG("Changed to directory %s", filename);
    } else if (str_begin_with(buf, "PORT") == 0) {
        int i;
        int portcom[6];
        char tmpip[100];

        i = 0;
        portcom[i++] = atoi(strtok(parameter_ptr, ".,;()"));
        for(; i < 6; i++)
            portcom[i] = atoi(strtok(0, ".,;()"));
        sprintf(tmpip, "%d.%d.%d.%d", portcom[0], portcom[1], portcom[2], portcom[3]);

        FD_ZERO(&readfds);
        if (-1 != session->pasv_sockfd)
            closesocket(session->pasv_sockfd);
        if ((session->pasv_sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
            sprintf(sbuf, "425 Can't open data connection %d.\r\n", __LINE__);
            send(session->sockfd, sbuf, strlen(sbuf), 0);
            closesocket(session->pasv_sockfd);
            session->pasv_sockfd = -1;
            session->pasv_active = 0;
            tls_mem_free(sbuf);
            return 0;
        }
        FTPS_DBG("pacvfd=%d\r\n", session->pasv_sockfd);
        pasvremote.sin_addr.s_addr = inet_addr(tmpip);
        pasvremote.sin_port = htons(portcom[4] * 256 + portcom[5]);
        pasvremote.sin_family = PF_INET;
        if (connect(session->pasv_sockfd, (struct sockaddr *)&pasvremote, addr_len) == -1) {
            // is it only local address?try using gloal ip addr
            FTPS_DBG("connect falied\r\n");
            pasvremote.sin_addr = session->remote.sin_addr;
            if (connect(session->pasv_sockfd, (struct sockaddr *)&pasvremote, addr_len) == -1) {
                sprintf(sbuf, "425 Can't open data connection %d.\r\n", __LINE__);
                send(session->sockfd, sbuf, strlen(sbuf), 0);
                closesocket(session->pasv_sockfd);
                session->pasv_sockfd = -1;
                tls_mem_free(sbuf);
                return 0;
            }
        }
        session->pasv_active = 1;
        session->pasv_port = portcom[4] * 256 + portcom[5];
        FTPS_DBG("Connected to Data(PORT) %s @ %d\n", tmpip, portcom[4] * 256 + portcom[5]);
        sprintf(sbuf, "200 Port Command Successful.\r\n");
        send(session->sockfd, sbuf, strlen(sbuf), 0);
    } else if (str_begin_with(buf, "REST") == 0) {
        if (atoi(parameter_ptr) >= 0) {
            session->offset = atoi(parameter_ptr);
            sprintf(sbuf, "350 Send RETR or STOR to start transfert.\r\n");
            send(session->sockfd, sbuf, strlen(sbuf), 0);
        }
    } else if (str_begin_with(buf, "MKD") == 0) {
        if (session->is_anonymous == TRUE) {
            sprintf(sbuf, "550 Permission denied.\r\n");
            send(session->sockfd, sbuf, strlen(sbuf), 0);
            tls_mem_free(sbuf);
            return 0;
        }

        build_full_path(session, parameter_ptr, filename, 256);

        fs_user_mount_t *vfs_fat = spi_fls_vfs_fat;
        FILINFO fno;
        FRESULT res = f_mkdir (&vfs_fat->fatfs, filename);

        if (FR_OK != res) {
            sprintf(sbuf, "550 File \"%s\" exists.\r\n", filename);
            send(session->sockfd, sbuf, strlen(sbuf), 0);
        } else {
            sprintf(sbuf, "257 directory \"%s\" successfully created.\r\n", filename);
            send(session->sockfd, sbuf, strlen(sbuf), 0);
        }
    } else if (str_begin_with(buf, "DELE") == 0) {
        if (session->is_anonymous == TRUE) {
            sprintf(sbuf, "550 Permission denied.\r\n");
            send(session->sockfd, sbuf, strlen(sbuf), 0);
            tls_mem_free(sbuf);
            return 0;
        }

        build_full_path(session, parameter_ptr, filename, 256);

        fs_user_mount_t *vfs_fat = spi_fls_vfs_fat;
        FILINFO fno;
        FRESULT res = f_unlink (&vfs_fat->fatfs, filename);

        if (FR_OK == res) {
            sprintf(sbuf, "250 Successfully deleted file \"%s\".\r\n", filename);
        } else {
            sprintf(sbuf, "550 Not such file or directory: %s.\r\n", filename);
        }
        send(session->sockfd, sbuf, strlen(sbuf), 0);
    } else if (str_begin_with(buf, "RMD") == 0) {
        if (session->is_anonymous == TRUE) {
            sprintf(sbuf, "550 Permission denied.\r\n");
            send(session->sockfd, sbuf, strlen(sbuf), 0);
            tls_mem_free(sbuf);
            return 0;
        }
        build_full_path(session, parameter_ptr, filename, 256);

        fs_user_mount_t *vfs_fat = spi_fls_vfs_fat;
        FILINFO fno;
        FRESULT res = f_unlink (&vfs_fat->fatfs, filename);

        if (FR_OK != res) {
            sprintf(sbuf, "550 Directory \"%s\" doesn't exist.\r\n", filename);
            send(session->sockfd, sbuf, strlen(sbuf), 0);
        } else {
            sprintf(sbuf, "257 directory \"%s\" successfully deleted.\r\n", filename);
            send(session->sockfd, sbuf, strlen(sbuf), 0);
        }
    } else if (str_begin_with(buf, "RNFR") == 0) {
        if (session->is_anonymous == TRUE) {
            sprintf(sbuf, "550 Permission denied.\r\n");
            send(session->sockfd, sbuf, strlen(sbuf), 0);
            tls_mem_free(sbuf);
            return 0;
        }

        strcpy(session->rename, buf + 5);
        sprintf(sbuf, "350 Requested file action pending further information.\r\n");
        send(session->sockfd, sbuf, strlen(sbuf), 0);
    } else if (str_begin_with(buf, "RNTO") == 0) {
        FILINFO fno;
        fs_user_mount_t *vfs_fat = spi_fls_vfs_fat;
        FRESULT res = f_rename (&vfs_fat->fatfs, session->rename, buf + 5);
        if (res != FR_OK) {
            sprintf(sbuf, "550 rename err.\r\n");
        } else {
            sprintf(sbuf, "200 command successful.\r\n");
        }
        send(session->sockfd, sbuf, strlen(sbuf), 0);
    } else if (str_begin_with(buf, "QUIT") == 0) {
        sprintf(sbuf, "221 Bye!\r\n");
        send(session->sockfd, sbuf, strlen(sbuf), 0);
        tls_mem_free(sbuf);
        return -1;
    } else if ((str_begin_with(buf, "NOOP") == 0) || (str_begin_with(buf, "noop") == 0)) {
        sprintf(sbuf, "200 Command okay.\r\n");
        send(session->sockfd, sbuf, strlen(sbuf), 0);
    } else {
        sprintf(sbuf, "502 Not Implemented.\r\n");
        send(session->sockfd, sbuf, strlen(sbuf), 0);
    }
    tls_mem_free(sbuf);
    return 0;
}

#ifdef __cplusplus
}
#endif
