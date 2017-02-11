#ifndef _SERVICEFS_PRIVATE_H
#define _SERVICEFS_PRIVATE_H

#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/idr.h>
#include <linux/uio.h>
#include <linux/wait.h>
#include <linux/kref.h>
#include <linux/fs.h>

#include "iov_buffer.h"

struct service {
	atomic_t             s_count;
	struct mutex         s_mutex;          // protects lists and allocators

	struct idr           s_channel_idr;    // channel id allocator
	int                  s_channel_start;
	struct idr           s_message_idr;    // message id allocator
	int                  s_message_start;

	struct list_head     s_channels;       // connected channels

	struct list_head     s_impulses;       // pending async messages
	struct list_head     s_messages;       // pending sync messages (blocked client threads)
	struct list_head     s_active;         // active sync messages (received but not completed)

	wait_queue_head_t    s_wqreceivers;    // wait queue for message receive
	wait_queue_head_t    s_wqselect;       // wait queue for poll/select

#define SERVICE_FLAGS_CANCELED             (1<<0)
#define SERVICE_FLAGS_OPEN_NOTIFY          (1<<1)
#define SERVICE_FLAGS_CLOSE_NOTIFY         (1<<2)
#define SERVICE_FLAGS_DEFAULT              \
		(SERVICE_FLAGS_OPEN_NOTIFY | SERVICE_FLAGS_CLOSE_NOTIFY)
	int                  s_flags;

	// TODO(eieio): figure out what to do about forking/execing
	void __user *        s_context;        // userspace context pointer
	struct file *        s_filp;           // does not hold a ref to the file
};

struct channel {
	struct service *     c_service;
	struct list_head     c_channels_node;  // hangs on s_channels

	int                  c_id;

	// TODO(eieio): consider adding a c_mutex member to protect these
	long                 c_events;         // events for poll/select
	wait_queue_head_t    c_waitqueue;      // wait queue for poll/select

#define CHANNEL_FLAGS_CANCELED             (1<<0)
#define CHANNEL_FLAGS_THREAD_POOL          (1<<1)
	int                  c_flags;

	// TODO(eieio): figure out what to do about forking/execing
	void __user *        c_context;
};

struct message {
	struct list_head     m_messages_node;  // hangs on s_messages or s_active

#define MESSAGE_NO_ID                     (-1)
	int                  m_id;

	struct kref          m_ref;            // protected by service.s_mutex

	int                  m_priority;       // boost priority
	struct task_struct * m_task;           // blocked client task
	pid_t                m_pid;            // sender tgid
	pid_t                m_tid;            // sender pid
	uid_t                m_euid;           // sender euid
	gid_t                m_egid;           // sender egid
	wait_queue_head_t    m_waitqueue;      // wait queue for sender

	struct service *     m_service;
	struct channel *     m_channel;

	int                  m_op;

	/* state below may be modified by multiple service threads */
	struct mutex         m_mutex;          // sync service access

	struct iov_buffer    m_sbuf;           // send buffer vecs
	struct iov_buffer    m_rbuf;           // receive buffer vecs

	const int *          m_fds;
	size_t               m_fdcnt;

	bool                 m_completed;
	bool                 m_interrupted;
	ssize_t              m_status;         // return code
};


struct impulse {
	struct list_head     i_impulses_node;  // hangs on s_impulses

	pid_t                i_pid;            // sender tgid
	pid_t                i_tid;            // sender pid
	uid_t                i_euid;           // sender euid
	gid_t                i_egid;           // sender egid

	struct service *     i_service;
	struct channel *     i_channel;

	int                  i_op;
	long                 i_data[4];
	size_t               i_len;
};

/*
 * Initialize caches.
 */
int servicefs_cache_init(void);

/*
 * Creation, status, and removal of services.
 */
struct service *service_new(void);
int service_cancel(struct service *svc);
void service_free(struct service *svc);

static inline bool __is_service_canceled(struct service *svc)
{
	return !!(svc->s_flags & SERVICE_FLAGS_CANCELED);
}

/*
 * Creation, status, and removal of channels.
 */
struct channel *channel_new(struct service *svc);
void __channel_cancel(struct channel *c);
void channel_remove(struct channel *c);

static inline bool __is_channel_canceled(struct channel *c)
{
	return !!(c->c_flags & CHANNEL_FLAGS_CANCELED);
}

/*
 * Status and removal of messages.
 */
void __message_complete(struct message *msg, int retcode);
void __message_cancel(struct message *m);

static inline bool __is_message_completed(struct message *m)
{
	return m->m_completed;
}

static inline bool is_message_completed(struct message *m)
{
	bool completed;

	mutex_lock(&m->m_mutex);
	completed = __is_message_completed(m);
	mutex_unlock(&m->m_mutex);

	return completed;
}

static inline bool __is_message_interrupted(struct message *m)
{
	return m->m_interrupted;
}

static inline bool __is_message_active(struct message *m)
{
	return m->m_id != MESSAGE_NO_ID;
}

static inline bool __is_message_detached(struct message *m)
{
	return m->m_channel == NULL;
}

void __impulse_cancel(struct impulse *i);

/*
 * Removal of a service's dentry.
 */
void servicefs_remove_dentry(struct dentry *dentry);

/*
 * Utility to create a new channel and its associated file.
 */
struct file *servicefs_create_channel(struct file *svc_file, int flags);
void servicefs_complete_channel_setup(struct file *filp);

/*
 * Get a channel struct from a file struct, if it's actually a channel.
 */
struct channel *servicefs_get_channel_from_file(struct file *filp);

/*
 * Get a service struct from a file struct, if it's actually a service.
 */
struct service *servicefs_get_service_from_file(struct file *filp);

/*
 * Service API handlers. Called by service_ioctl().
 */
struct servicefs_msg_info_struct;

int servicefs_push_channel(struct service *svc, int svcfd, int msgid,
		int flags, int __user *cid, void __user *ctx, bool is_compat);
int servicefs_close_channel(struct service *svc, int cid);
int servicefs_check_channel(struct service *svc, int svcfd, int msgid,
		int index, int __user *cid, void __user **ctx, bool is_compat);

int servicefs_set_service_context(struct service *svc, void __user *ctx);
int servicefs_set_channel_context(struct service *svc, int cid, void __user *ctx);
int servicefs_msg_recv(struct service *svc,
		struct servicefs_msg_info_struct __user *msg_info, long timeout,
		bool is_compat);
ssize_t servicefs_msg_readv(struct service *svc, int msgid,
		const iov *vec, size_t cnt);
ssize_t servicefs_msg_writev(struct service *svc, int msgid,
		const iov *vec, size_t cnt);
int servicefs_msg_seek(struct service *svc, int msgid, long offset, int whence);
ssize_t servicefs_msg_busv(struct service *svc, int dst_msgid, long dst_off,
		int src_msgid, long src_off, size_t len);
int servicefs_msg_reply(struct service *svc, int msgid, ssize_t retcode);
int servicefs_msg_reply_fd(struct service *svc, int msgid, unsigned int pushfd);
int servicefs_mod_channel_events(struct service *svc, int cid,
		int clr, int set);
int servicefs_msg_push_fd(struct service *svc, int msgid, unsigned int pushfd);
int servicefs_msg_get_fd(struct service *svc, int msgid, unsigned int index);

/*
 * Client API handlers. Called by client_ioctl() and various client file ops.
 */
ssize_t servicefs_msg_sendv(struct channel *c, int op, const iov *svec, size_t scnt,
		const iov *rvec, size_t rcnt, const int *fds, size_t fdcnt, long task_state);
int servicefs_msg_send_impulse(struct channel *c, int op,
		void __user *buf, size_t len);

static inline ssize_t servicefs_msg_sendv_interruptible(struct channel *c, int op,
		const iov *svec, size_t scnt, const iov *rvec, size_t rcnt,
		const int *fds, size_t fdcnt)
{
	return servicefs_msg_sendv(c, op, svec, scnt, rvec, rcnt, fds, fdcnt,
			TASK_INTERRUPTIBLE);
}

static inline ssize_t servicefs_msg_sendv_uninterruptible(struct channel *c, int op,
		const iov *svec, size_t scnt, const iov *rvec, size_t rcnt,
		const int *fds, size_t fdcnt)
{
	return servicefs_msg_sendv(c, op, svec, scnt, rvec, rcnt, fds, fdcnt,
			TASK_UNINTERRUPTIBLE);
}
/*
 * Data transfer utilities for moving data between address spaces.
 */
ssize_t vm_transfer_to_remote(struct iov_buffer *remote, struct iov_buffer *local,
		struct task_struct *task, bool *remote_fault);
ssize_t vm_transfer_from_remote(struct iov_buffer *remote, struct iov_buffer *local,
		struct task_struct *task, bool *remote_fault);

/*
 * File descriptor and file object utilities. These are analogs of
 * common kernel functions, modified to act on the specified task
 * instead of implicitly on current.
 */
int servicefs_get_unused_fd_flags(struct task_struct *task, int flags);
void servicefs_fd_install(struct task_struct *task, unsigned int fd,
		struct file *file);
struct file *servicefs_fget(struct task_struct *task, unsigned int fd);

#endif

