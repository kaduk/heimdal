/*
 * Copyright (c) 1997 - 2004 Kungliga Tekniska H�gskolan
 * (Royal Institute of Technology, Stockholm, Sweden). 
 * All rights reserved. 
 *
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions 
 * are met: 
 *
 * 1. Redistributions of source code must retain the above copyright 
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright 
 *    notice, this list of conditions and the following disclaimer in the 
 *    documentation and/or other materials provided with the distribution. 
 *
 * 3. Neither the name of the Institute nor the names of its contributors 
 *    may be used to endorse or promote products derived from this software 
 *    without specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND 
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE 
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL 
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS 
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) 
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT 
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY 
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE. 
 */

#include "iprop.h"
#include <rtbl.h>

RCSID("$Id$");

static krb5_log_facility *log_facility;

const char *slave_stats_file = KADM5_SLAVE_STATS;
const char *slave_time_missing = "2 min";
const char *slave_time_gone = "5 min";

static int time_before_missing;
static int time_before_gone;

static int
make_signal_socket (krb5_context context)
{
    struct sockaddr_un addr;
    int fd;

    fd = socket (AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0)
	krb5_err (context, 1, errno, "socket AF_UNIX");
    memset (&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strlcpy (addr.sun_path, KADM5_LOG_SIGNAL, sizeof(addr.sun_path));
    unlink (addr.sun_path);
    if (bind (fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	krb5_err (context, 1, errno, "bind %s", addr.sun_path);
    return fd;
}

static int
make_listen_socket (krb5_context context, const char *port_str)
{
    int fd;
    int one = 1;
    struct sockaddr_in addr;

    fd = socket (AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
	krb5_err (context, 1, errno, "socket AF_INET");
    setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, (void *)&one, sizeof(one));
    memset (&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;

    if (port_str) {
	addr.sin_port = krb5_getportbyname (context,
					      port_str, "tcp", 
					      0);
	if (addr.sin_port == 0) {
	    char *ptr;
	    long port;

	    port = strtol (port_str, &ptr, 10);
	    if (port == 0 && ptr == port_str)
		krb5_errx (context, 1, "bad port `%s'", port_str);
	    addr.sin_port = htons(port);
	}
    } else {
	addr.sin_port = krb5_getportbyname (context, IPROP_SERVICE, 
					    "tcp", IPROP_PORT);
    }
    if(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	krb5_err (context, 1, errno, "bind");
    if (listen(fd, SOMAXCONN) < 0)
	krb5_err (context, 1, errno, "listen");
    return fd;
}

struct slave {
    int fd;
    struct sockaddr_in addr;
    char *name;
    krb5_auth_context ac;
    u_int32_t version;
    time_t seen;
    unsigned long flags;
#define SLAVE_F_DEAD	0x1
#define SLAVE_F_AYT	0x2
    struct slave *next;
};

typedef struct slave slave;

static int
check_acl (krb5_context context, const char *name)
{
    FILE *fp;
    char buf[256];
    int ret = 1;

    fp = fopen (KADM5_SLAVE_ACL, "r");
    if (fp == NULL)
	return 1;
    while (fgets(buf, sizeof(buf), fp) != NULL) {
	if (buf[strlen(buf) - 1 ] == '\n')
	    buf[strlen(buf) - 1 ] = '\0';
	if (strcmp (buf, name) == 0) {
	    ret = 0;
	    break;
	}
    }
    fclose (fp);
    return ret;
}

static void
slave_seen(slave *s)
{
    s->flags &= ~SLAVE_F_AYT;
    s->seen = time(NULL);
}

static int
slave_missing_p (slave *s)
{
    if (time(NULL) > s->seen + time_before_missing)
	return 1;
    return 0;
}

static int
slave_gone_p (slave *s)
{
    if (time(NULL) > s->seen + time_before_gone)
	return 1;
    return 0;
}

static void
slave_dead(slave *s)
{
    if (s->fd >= 0) {
	close (s->fd);
	s->fd = -1;
    }
    s->flags |= SLAVE_F_DEAD;
    slave_seen(s);
}

static void
remove_slave (krb5_context context, slave *s, slave **root)
{
    slave **p;

    if (s->fd >= 0)
	close (s->fd);
    if (s->name)
	free (s->name);
    if (s->ac)
	krb5_auth_con_free (context, s->ac);

    for (p = root; *p; p = &(*p)->next)
	if (*p == s) {
	    *p = s->next;
	    break;
	}
    free (s);
}

static void
add_slave (krb5_context context, krb5_keytab keytab, slave **root, int fd)
{
    krb5_principal server;
    krb5_error_code ret;
    slave *s;
    socklen_t addr_len;
    krb5_ticket *ticket = NULL;
    char hostname[128];

    s = malloc(sizeof(*s));
    if (s == NULL) {
	krb5_warnx (context, "add_slave: no memory");
	return;
    }
    s->name = NULL;
    s->ac = NULL;

    addr_len = sizeof(s->addr);
    s->fd = accept (fd, (struct sockaddr *)&s->addr, &addr_len);
    if (s->fd < 0) {
	krb5_warn (context, errno, "accept");
	goto error;
    }
    gethostname(hostname, sizeof(hostname));
    ret = krb5_sname_to_principal (context, hostname, IPROP_NAME,
				   KRB5_NT_SRV_HST, &server);
    if (ret) {
	krb5_warn (context, ret, "krb5_sname_to_principal");
	goto error;
    }

    ret = krb5_recvauth (context, &s->ac, &s->fd,
			 IPROP_VERSION, server, 0, keytab, &ticket);
    krb5_free_principal (context, server);
    if (ret) {
	krb5_warn (context, ret, "krb5_recvauth");
	goto error;
    }
    ret = krb5_unparse_name (context, ticket->client, &s->name);
    if (ret) {
	krb5_warn (context, ret, "krb5_unparse_name");
	goto error;
    }
    if (check_acl (context, s->name)) {
	krb5_warnx (context, "%s not in acl", s->name);
	goto error;
    }
    krb5_free_ticket (context, ticket);
    ticket = NULL;

    {
	slave *l = *root;

	while (l) {
	    if (strcmp(l->name, s->name) == 0)
		break;
	    l = l->next;
	}
	if (l) {
	    if (l->flags & SLAVE_F_DEAD) {
		remove_slave(context, l, root);
	    } else {
		krb5_warnx (context, "second connection from %s", s->name);
		goto error;
	    }
	}
    }

    krb5_warnx (context, "connection from %s", s->name);

    s->version = 0;
    s->flags = 0;
    slave_seen(s);
    s->next = *root;
    *root = s;
    return;
error:
    remove_slave(context, s, root);
}

struct prop_context {
    krb5_auth_context auth_context;
    int fd;
};

static int
prop_one (krb5_context context, HDB *db, hdb_entry *entry, void *v)
{
    krb5_error_code ret;
    krb5_storage *sp;
    krb5_data data;
    struct slave *slave = (struct slave *)v;

    ret = hdb_entry2value (context, entry, &data);
    if (ret)
	return ret;
    ret = krb5_data_realloc (&data, data.length + 4);
    if (ret) {
	krb5_data_free (&data);
	return ret;
    }
    memmove ((char *)data.data + 4, data.data, data.length - 4);
    sp = krb5_storage_from_data(&data);
    if (sp == NULL) {
	krb5_data_free (&data);
	return ENOMEM;
    }
    krb5_store_int32(sp, ONE_PRINC);
    krb5_storage_free(sp);

    ret = krb5_write_priv_message (context, slave->ac, &slave->fd, &data);
    krb5_data_free (&data);
    return ret;
}

static int
send_complete (krb5_context context, slave *s,
	       const char *database, u_int32_t current_version)
{
    krb5_error_code ret;
    krb5_storage *sp;
    HDB *db;
    krb5_data data;
    char buf[8];

    ret = hdb_create (context, &db, database);
    if (ret)
	krb5_err (context, 1, ret, "hdb_create: %s", database);
    ret = db->hdb_open (context, db, O_RDONLY, 0);
    if (ret)
	krb5_err (context, 1, ret, "db->open");

    sp = krb5_storage_from_mem (buf, 4);
    if (sp == NULL)
	krb5_errx (context, 1, "krb5_storage_from_mem");
    krb5_store_int32 (sp, TELL_YOU_EVERYTHING);
    krb5_storage_free (sp);

    data.data   = buf;
    data.length = 4;

    ret = krb5_write_priv_message(context, s->ac, &s->fd, &data);

    if (ret) {
	krb5_warn (context, ret, "krb5_write_priv_message");
	slave_dead(s);
	return ret;
    }

    ret = hdb_foreach (context, db, 0, prop_one, s);
    if (ret) {
	krb5_warn (context, ret, "hdb_foreach");
	slave_dead(s);
	return ret;
    }

    (*db->hdb_close)(context, db);
    (*db->hdb_destroy)(context, db);

    sp = krb5_storage_from_mem (buf, 8);
    if (sp == NULL)
	krb5_errx (context, 1, "krb5_storage_from_mem");
    krb5_store_int32 (sp, NOW_YOU_HAVE);
    krb5_store_int32 (sp, current_version);
    krb5_storage_free (sp);

    data.length = 8;

    s->version = current_version;

    ret = krb5_write_priv_message(context, s->ac, &s->fd, &data);
    if (ret) {
	slave_dead(s);
	krb5_warn (context, ret, "krb5_write_priv_message");
	return ret;
    }

    slave_seen(s);

    return 0;
}

static int
send_are_you_there (krb5_context context, slave *s)
{
    krb5_storage *sp;
    krb5_data data;
    char buf[4];
    int ret;

    if (s->flags & (SLAVE_F_DEAD|SLAVE_F_AYT))
	return 0;

    s->flags |= SLAVE_F_AYT;

    data.data = buf;
    data.length = 4;

    sp = krb5_storage_from_mem (buf, 4);
    if (sp == NULL) {
	krb5_warnx (context, "are_you_there: krb5_data_alloc");
	slave_dead(s);
	return 1;
    }
    krb5_store_int32 (sp, ARE_YOU_THERE);
    krb5_storage_free (sp);

    ret = krb5_write_priv_message(context, s->ac, &s->fd, &data);

    if (ret) {
	krb5_warn (context, ret, "are_you_there: krb5_write_priv_message");
	slave_dead(s);
	return 1;
    }

    return 0;
}

static int
send_diffs (krb5_context context, slave *s, int log_fd,
	    const char *database, u_int32_t current_version)
{
    krb5_storage *sp;
    u_int32_t ver;
    time_t timestamp;
    enum kadm_ops op;
    u_int32_t len;
    off_t right, left;
    krb5_data data;
    int ret = 0;

    if (s->version == current_version)
	return 0;

    if (s->flags & SLAVE_F_DEAD)
	return 0;

    sp = kadm5_log_goto_end (log_fd);
    right = krb5_storage_seek(sp, 0, SEEK_CUR);
    for (;;) {
	if (kadm5_log_previous (sp, &ver, &timestamp, &op, &len))
	    abort ();
	left = krb5_storage_seek(sp, -16, SEEK_CUR);
	if (ver == s->version)
	    return 0;
	if (ver == s->version + 1)
	    break;
	if (left == 0)
	    return send_complete (context, s, database, current_version);
    }
    ret = krb5_data_alloc (&data, right - left + 4);
    if (ret) {
	krb5_warn (context, ret, "send_diffs: krb5_data_alloc");
	slave_dead(s);
	return 1;
    }
    krb5_storage_read (sp, (char *)data.data + 4, data.length - 4);
    krb5_storage_free(sp);

    sp = krb5_storage_from_data (&data);
    if (sp == NULL) {
	krb5_warnx (context, "send_diffs: krb5_storage_from_data");
	slave_dead(s);
	return 1;
    }
    krb5_store_int32 (sp, FOR_YOU);
    krb5_storage_free(sp);

    ret = krb5_write_priv_message(context, s->ac, &s->fd, &data);
    krb5_data_free(&data);

    if (ret) {
	krb5_warn (context, ret, "send_diffs: krb5_write_priv_message");
	slave_dead(s);
	return 1;
    }
    slave_seen(s);

    return 0;
}

static int
process_msg (krb5_context context, slave *s, int log_fd,
	     const char *database, u_int32_t current_version)
{
    int ret = 0;
    krb5_data out;
    krb5_storage *sp;
    int32_t tmp;

    ret = krb5_read_priv_message(context, s->ac, &s->fd, &out);
    if(ret) {
	krb5_warn (context, ret, "error reading message from %s", s->name);
	return 1;
    }

    sp = krb5_storage_from_mem (out.data, out.length);
    if (sp == NULL) {
	krb5_warnx (context, "process_msg: no memory");
	krb5_data_free (&out);
	return 1;
    }
    if (krb5_ret_int32 (sp, &tmp) != 0) {
	krb5_warnx (context, "process_msg: client send too short command");
	krb5_data_free (&out);
	return 1;
    }
    switch (tmp) {
    case I_HAVE :
	ret = krb5_ret_int32 (sp, &tmp);
	if (ret != 0) {
	    krb5_warnx (context, "process_msg: client send too I_HAVE data");
	    break;
	}
	s->version = tmp;
	ret = send_diffs (context, s, log_fd, database, current_version);
	break;
    case I_AM_HERE :
	break;
    case ARE_YOU_THERE:
    case FOR_YOU :
    default :
	krb5_warnx (context, "Ignoring command %d", tmp);
	break;
    }

    krb5_data_free (&out);

    slave_seen(s);

    return ret;
}

#define SLAVE_NAME	"Name"
#define SLAVE_ADDRESS	"Address"
#define SLAVE_VERSION	"Version"
#define SLAVE_STATUS	"Status"
#define SLAVE_SEEN	"Last Seen"

static void
write_stats(krb5_context context, slave *slaves, u_int32_t current_version)
{
    char str[100];
    rtbl_t tbl;
    time_t t = time(NULL);
    FILE *fp;

    fp = fopen(slave_stats_file, "w");
    if (fp == NULL)
	return;

    krb5_format_time(context, t, str, sizeof(str), TRUE); 
    fprintf(fp, "Status for slaves, last updated: %s\n\n", str);

    fprintf(fp, "Master version: %lu\n\n", (unsigned long)current_version);

    tbl = rtbl_create();
    if (tbl == NULL) {
	fclose(fp);
	return;
    }

    rtbl_add_column(tbl, SLAVE_NAME, 0);
    rtbl_add_column(tbl, SLAVE_ADDRESS, 0);
    rtbl_add_column(tbl, SLAVE_VERSION, RTBL_ALIGN_RIGHT);
    rtbl_add_column(tbl, SLAVE_STATUS, 0);
    rtbl_add_column(tbl, SLAVE_SEEN, 0);

    rtbl_set_prefix(tbl, "  ");
    rtbl_set_column_prefix(tbl, SLAVE_NAME, "");

    while (slaves) {
	krb5_address addr;
	krb5_error_code ret;
	rtbl_add_column_entry(tbl, SLAVE_NAME, slaves->name);
	ret = krb5_sockaddr2address (context, 
				     (struct sockaddr*)&slaves->addr, &addr);
	if(ret == 0) {
	    krb5_print_address(&addr, str, sizeof(str), NULL);
	    krb5_free_address(context, &addr);
	    rtbl_add_column_entry(tbl, SLAVE_ADDRESS, str);
	} else
	    rtbl_add_column_entry(tbl, SLAVE_ADDRESS, "<unknown>");
	
	snprintf(str, sizeof(str), "%u", (unsigned)slaves->version);
	rtbl_add_column_entry(tbl, SLAVE_VERSION, str);

	if (slaves->flags & SLAVE_F_DEAD)
	    rtbl_add_column_entry(tbl, SLAVE_STATUS, "Down");
	else
	    rtbl_add_column_entry(tbl, SLAVE_STATUS, "Up");

	ret = krb5_format_time(context, slaves->seen, str, sizeof(str), TRUE); 
	rtbl_add_column_entry(tbl, SLAVE_SEEN, str);

	slaves = slaves->next;
    }

    rtbl_format(tbl, fp);
    rtbl_destroy(tbl);

    fclose(fp);
}


static char *realm;
static int version_flag;
static int help_flag;
static char *keytab_str = "HDB:";
static char *database;
static char *config_file;
static char *port_str;
#ifdef HAVE_DAEMON
static int detach_from_console = 0;
#endif

static struct getargs args[] = {
    { "config-file", 'c', arg_string, &config_file },
    { "realm", 'r', arg_string, &realm },
    { "keytab", 'k', arg_string, &keytab_str,
      "keytab to get authentication from", "kspec" },
    { "database", 'd', arg_string, &database, "database", "file"},
    { "slave-stats-file", 0, arg_string, &slave_stats_file, 
      "file for slave status information", "file"},
    { "time-missing", 0, arg_string, &slave_time_missing, 
      "time before slave is polled for presence", "time"},
    { "time-gone", 0, arg_string, &slave_time_gone,
      "time of inactivity after which a slave is considered gone", "time"},
    { "port", 0, arg_string, &port_str,
      "port ipropd will listen too", "port"},
#ifdef HAVE_DAEMON
    { "detach", 0, arg_flag, &detach_from_console, 
      "detach from console" },
#endif
    { "version", 0, arg_flag, &version_flag },
    { "help", 0, arg_flag, &help_flag }
};
static int num_args = sizeof(args) / sizeof(args[0]);

int
main(int argc, char **argv)
{
    krb5_error_code ret;
    krb5_context context;
    void *kadm_handle;
    kadm5_server_context *server_context;
    kadm5_config_params conf;
    int signal_fd, listen_fd;
    int log_fd;
    slave *slaves = NULL;
    u_int32_t current_version = 0, old_version = 0;
    krb5_keytab keytab;
    int optind;
    char **files;
    
    optind = krb5_program_setup(&context, argc, argv, args, num_args, NULL);
    
    if(help_flag)
	krb5_std_usage(0, args, num_args);
    if(version_flag) {
	print_version(NULL);
	exit(0);
    }

    if (config_file == NULL)
	config_file = HDB_DB_DIR "/kdc.conf";

    ret = krb5_prepend_config_files_default(config_file, &files);
    if (ret)
	krb5_err(context, 1, ret, "getting configuration files");

    ret = krb5_set_config_files(context, files);
    krb5_free_config_files(files);
    if (ret)
	krb5_err(context, 1, ret, "reading configuration files");

    time_before_gone = parse_time (slave_time_gone,  "s");
    if (time_before_gone < 0)
	krb5_errx (context, 1, "couldn't parse time: %s", slave_time_gone);
    time_before_missing = parse_time (slave_time_missing,  "s");
    if (time_before_missing < 0)
	krb5_errx (context, 1, "couldn't parse time: %s", slave_time_missing);

#ifdef HAVE_DAEMON
    if (detach_from_console)
	daemon(0, 0);
#endif
    pidfile (NULL);
    krb5_openlog (context, "ipropd-master", &log_facility);
    krb5_set_warn_dest(context, log_facility);

    ret = krb5_kt_register(context, &hdb_kt_ops);
    if(ret)
	krb5_err(context, 1, ret, "krb5_kt_register");

    ret = krb5_kt_resolve(context, keytab_str, &keytab);
    if(ret)
	krb5_err(context, 1, ret, "krb5_kt_resolve: %s", keytab_str);
    
    memset(&conf, 0, sizeof(conf));
    if(realm) {
	conf.mask |= KADM5_CONFIG_REALM;
	conf.realm = realm;
    }
    ret = kadm5_init_with_skey_ctx (context,
				    KADM5_ADMIN_SERVICE,
				    NULL,
				    KADM5_ADMIN_SERVICE,
				    &conf, 0, 0, 
				    &kadm_handle);
    if (ret)
	krb5_err (context, 1, ret, "kadm5_init_with_password_ctx");

    server_context = (kadm5_server_context *)kadm_handle;

    log_fd = open (server_context->log_context.log_file, O_RDONLY, 0);
    if (log_fd < 0)
	krb5_err (context, 1, errno, "open %s",
		  server_context->log_context.log_file);

    signal_fd = make_signal_socket (context);
    listen_fd = make_listen_socket (context, port_str);

    signal (SIGPIPE, SIG_IGN);

    for (;;) {
	slave *p;
	fd_set readset;
	int max_fd = 0;
	struct timeval to = {30, 0};
	u_int32_t vers;

	if (signal_fd >= FD_SETSIZE || listen_fd >= FD_SETSIZE)
	    krb5_errx (context, 1, "fd too large");

	FD_ZERO(&readset);
	FD_SET(signal_fd, &readset);
	max_fd = max(max_fd, signal_fd);
	FD_SET(listen_fd, &readset);
	max_fd = max(max_fd, listen_fd);

	for (p = slaves; p != NULL; p = p->next) {
	    if (p->flags & SLAVE_F_DEAD)
		continue;
	    FD_SET(p->fd, &readset);
	    max_fd = max(max_fd, p->fd);
	}

	ret = select (max_fd + 1,
		      &readset, NULL, NULL, &to);
	if (ret < 0) {
	    if (errno == EINTR)
		continue;
	    else
		krb5_err (context, 1, errno, "select");
	}

	if (ret == 0) {
	    old_version = current_version;
	    kadm5_log_get_version_fd (log_fd, &current_version);

	    if (current_version > old_version) {
		for (p = slaves; p != NULL; p = p->next) {
		    if (p->flags & SLAVE_F_DEAD)
			continue;
		    send_diffs (context, p, log_fd, database, current_version);
		}
	    }
	}

	if (ret && FD_ISSET(signal_fd, &readset)) {
	    struct sockaddr_un peer_addr;
	    socklen_t peer_len = sizeof(peer_addr);

	    if(recvfrom(signal_fd, (void *)&vers, sizeof(vers), 0,
			(struct sockaddr *)&peer_addr, &peer_len) < 0) {
		krb5_warn (context, errno, "recvfrom");
		continue;
	    }
	    --ret;
	    assert(ret >= 0);
	    old_version = current_version;
	    kadm5_log_get_version_fd (log_fd, &current_version);
	    for (p = slaves; p != NULL; p = p->next)
		send_diffs (context, p, log_fd, database, current_version);
        }

	for(p = slaves; p != NULL; p = p->next) {
	    if (p->flags & SLAVE_F_DEAD)
	        continue;
	    if (ret && FD_ISSET(p->fd, &readset)) {
		--ret;
		assert(ret >= 0);
		if(process_msg (context, p, log_fd, database, current_version))
		    slave_dead(p);
	    } else if (slave_gone_p (p))
		slave_dead (p);
	    else if (slave_missing_p (p))
		send_are_you_there (context, p);
	}

	if (ret && FD_ISSET(listen_fd, &readset)) {
	    add_slave (context, keytab, &slaves, listen_fd);
	    --ret;
	    assert(ret >= 0);
	}
	write_stats(context, slaves, current_version);
    }

    return 0;
}
