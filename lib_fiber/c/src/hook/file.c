#include "stdafx.h"

#ifdef	HAS_IO_URING

#include "common.h"
#include "event.h"
#include "fiber.h"
#include "hook.h"

#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "../event/event_io_uring.h"

#define	CHECK_API(name, fn) do {  \
	if ((fn) == NULL) {  \
		hook_once();  \
		if ((fn) == NULL) {  \
			msg_error("%s: %s NULL", __FUNCTION__, (name));  \
			return -1;  \
		}  \
	}  \
} while (0)

#define	FILE_ALLOC(__fe, __type) do {  \
	(__fe) = file_event_alloc(-1);  \
	(__fe)->fiber_r->status = FIBER_STATUS_NONE;  \
	(__fe)->fiber_w->status = FIBER_STATUS_NONE;  \
	(__fe)->r_proc = file_read_callback;  \
	(__fe)->mask   = (__type);  \
	(__fe)->type   = TYPE_EVENTABLE | TYPE_FILE;  \
} while (0)

static void file_read_callback(EVENT *ev UNUSED, FILE_EVENT *fe)
{
	if (fe->fiber_r->status != FIBER_STATUS_READY) {
		acl_fiber_ready(fe->fiber_r);
	}
}

int file_close(EVENT *ev, FILE_EVENT *fe)
{
	CHECK_API("sys_close", sys_close);

	if (!var_hook_sys_api) {
		return (*sys_close)(fe->fd);
	}

	if (!EVENT_IS_IO_URING(ev)) {
		return (*sys_close)(fe->fd);
	}

	fe->fiber_r         = acl_fiber_running();
	fe->fiber_r->status = FIBER_STATUS_NONE;
	fe->r_proc          = file_read_callback;
	fe->mask           |= EVENT_FILE_CLOSE;

	event_uring_file_close(ev, fe);

	ev->waiter++;
	acl_fiber_switch();
	ev->waiter--;

	fe->mask &= ~EVENT_FILE_CLOSE;

	if (fe->reader_ctx.res == 0) {
		return 0;
	} else {
		acl_fiber_set_error(-fe->reader_ctx.res);
		return -1;
	}
}

int openat(int dirfd, const char *pathname, int flags, ...)
{
	FILE_EVENT *fe;
	EVENT *ev;
	mode_t mode;
	va_list ap;

	va_start(ap, flags);
	mode = va_arg(ap, mode_t);
	va_end(ap);

	CHECK_API("sys_openat", sys_openat);

	if (!var_hook_sys_api) {
		return (*sys_openat)(dirfd, pathname, flags, mode);
	}

	ev = fiber_io_event();
	if (!EVENT_IS_IO_URING(ev)) {
		return (*sys_openat)(dirfd, pathname, flags, mode);
	}

	FILE_ALLOC(fe, EVENT_FILE_OPENAT);
	fe->var.path = strdup(pathname);

	event_uring_file_openat(ev, fe, dirfd, fe->var.path, flags, mode);

	ev->waiter++;
	acl_fiber_switch();
	ev->waiter--;

	fe->mask &= ~EVENT_FILE_OPENAT;
	free(fe->var.path);
	fe->var.path = NULL;

	if (fe->reader_ctx.res >= 0) {
		fe->fd   = fe->reader_ctx.res;
		fe->type = TYPE_FILE | TYPE_EVENTABLE;
		fiber_file_set(fe);  // Save the fe for the future using.
		return fe->fd;
	}

	acl_fiber_set_error(-fe->reader_ctx.res);
	file_event_unrefer(fe);
	return -1;
}

int open(const char *pathname, int flags, ...)
{
	mode_t mode;
	va_list ap;

	va_start(ap, flags);
	mode = va_arg(ap, mode_t);
	va_end(ap);

	return openat(AT_FDCWD, pathname, flags, mode);
}

int unlink(const char *pathname)
{
	FILE_EVENT *fe;
	EVENT *ev;

	CHECK_API("sys_unlink", sys_unlink);

	if (!var_hook_sys_api) {
		return (*sys_unlink)(pathname);
	}

	ev = fiber_io_event();
	if (!EVENT_IS_IO_URING(ev)) {
		return (*sys_unlink)(pathname);
	}

	FILE_ALLOC(fe, EVENT_FILE_UNLINK);
	fe->var.path = strdup(pathname);

	event_uring_file_unlink(ev, fe, fe->var.path);

	ev->waiter++;
	acl_fiber_switch();
	ev->waiter--;

	fe->mask &= ~EVENT_FILE_UNLINK;
	free(fe->var.path);
	fe->var.path = NULL;

	if (fe->reader_ctx.res == 0) {
		file_event_unrefer(fe);
		return 0;
	} else {
		acl_fiber_set_error(-fe->reader_ctx.res);
		file_event_unrefer(fe);
		return -1;
	}
}

int renameat2(int olddirfd, const char *oldpath,
	int newdirfd, const char *newpath, unsigned int flags)
{
	FILE_EVENT *fe;
	EVENT *ev;

	CHECK_API("sys_renameat2", sys_renameat2);

	if (!var_hook_sys_api) {
		return (*sys_renameat2)(olddirfd, oldpath, newdirfd, newpath, flags);
	}

	ev = fiber_io_event();
	if (!EVENT_IS_IO_URING(ev)) {
		return (*sys_renameat2)(olddirfd, oldpath, newdirfd, newpath, flags);
	}

	FILE_ALLOC(fe, EVENT_FILE_RENAMEAT2);
	fe->in.read_ctx.buf = strdup(oldpath);
	fe->var.path = strdup(newpath);

	event_uring_file_renameat2(ev, fe, olddirfd, fe->in.read_ctx.buf,
		newdirfd, fe->var.path, flags);

	ev->waiter++;
	acl_fiber_switch();
	ev->waiter--;

	fe->mask &= ~EVENT_FILE_RENAMEAT2;
	free(fe->in.read_ctx.buf);
	free(fe->var.path);

	if (fe->reader_ctx.res == 0) {
		file_event_unrefer(fe);
		return 0;
	} else {
		acl_fiber_set_error(-fe->reader_ctx.res);
		file_event_unrefer(fe);
		return -1;
	}
}

int renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath)
{
	return renameat2(olddirfd, oldpath, newdirfd, newpath, 0);
}

int rename(const char *oldpath, const char *newpath)
{
	return renameat(AT_FDCWD, oldpath, AT_FDCWD, newpath);
}

int statx(int dirfd, const char *pathname, int flags, unsigned int mask,
	struct statx *statxbuf)
{
	FILE_EVENT *fe;
	EVENT *ev;

	CHECK_API("sys_statx", sys_statx);

	if (!var_hook_sys_api) {
		return (*sys_statx)(dirfd, pathname, flags, mask, statxbuf);
	}

	ev = fiber_io_event();
	if (!EVENT_IS_IO_URING(ev)) {
		return (*sys_statx)(dirfd, pathname, flags, mask, statxbuf);
	}

	FILE_ALLOC(fe, EVENT_FILE_STATX);
	fe->in.read_ctx.buf = strdup(pathname);
	fe->var.statxbuf = (struct statx*) malloc(sizeof(struct statx));
	memcpy(fe->var.statxbuf, statxbuf, sizeof(struct statx));

	event_uring_file_statx(ev, fe, dirfd, fe->in.read_ctx.buf, flags, mask,
		fe->var.statxbuf);

	ev->waiter++;
	acl_fiber_switch();
	ev->waiter--;

	fe->mask &= ~EVENT_FILE_STATX;
	free(fe->in.read_ctx.buf);
	fe->in.read_ctx.buf = NULL;

	if (fe->reader_ctx.res == 0) {
		memcpy(statxbuf, fe->var.statxbuf, sizeof(struct statx));
		free(fe->var.statxbuf);
		file_event_unrefer(fe);
		return 0;
	} else {
		acl_fiber_set_error(-fe->reader_ctx.res);
		free(fe->var.statxbuf);
		file_event_unrefer(fe);
		return -1;
	}
}

int stat(const char *pathname, struct stat *statbuf)
{
	int flags = AT_STATX_SYNC_AS_STAT;
	unsigned int mask = STATX_ALL;
	struct statx statxbuf;

	if (statx(AT_FDCWD, pathname, flags, mask, &statxbuf) == -1) {
		return -1;
	}

	statbuf->st_dev         = statxbuf.stx_dev_major;
	statbuf->st_ino         = statxbuf.stx_ino;
	statbuf->st_mode        = statxbuf.stx_mode;
	statbuf->st_nlink       = statxbuf.stx_nlink;
	statbuf->st_uid         = statxbuf.stx_uid;
	statbuf->st_gid         = statxbuf.stx_gid;
	statbuf->st_rdev        = statxbuf.stx_rdev_major;
	statbuf->st_size        = statxbuf.stx_size;
	statbuf->st_blksize     = statxbuf.stx_blksize;
	statbuf->st_blocks      = statxbuf.stx_blocks;
	statbuf->st_atim.tv_sec = statxbuf.stx_atime.tv_sec;
	statbuf->st_mtim.tv_sec = statxbuf.stx_mtime.tv_sec;
	statbuf->st_ctim.tv_sec = statxbuf.stx_ctime.tv_sec;
	return 0;
} 

int mkdirat(int dirfd, const char *pathname, mode_t mode)
{
	FILE_EVENT *fe;
	EVENT *ev;

	CHECK_API("sys_mkdirat", sys_mkdirat);

	if (!var_hook_sys_api) {
		return (*sys_mkdirat)(dirfd, pathname, mode);
	}

	ev = fiber_io_event();
	if (!EVENT_IS_IO_URING(ev)) {
		return (*sys_mkdirat)(dirfd, pathname, mode);
	}

	FILE_ALLOC(fe, EVENT_DIR_MKDIRAT);
	fe->var.path = strdup(pathname);

	event_uring_mkdirat(ev, fe, dirfd, fe->var.path, mode);

	ev->waiter++;
	acl_fiber_switch();
	ev->waiter--;

	fe->mask &= ~EVENT_DIR_MKDIRAT;
	free(fe->var.path);

	if (fe->reader_ctx.res == 0) {
		file_event_unrefer(fe);
		return 0;
	} else {
		acl_fiber_set_error(-fe->reader_ctx.res);
		file_event_unrefer(fe);
		return -1;
	}
}

ssize_t pread(int fd, void *buf, size_t count, off_t offset)
{
	FILE_EVENT *fe;
	ssize_t ret;

	if (fd <= INVALID_SOCKET) {
		msg_error("%s: invalid fd: %d", __FUNCTION__, fd);
		return -1;
	}

	if (sys_pread == NULL) {
		hook_once();
	}

	if (!var_hook_sys_api) {
		return (*sys_pread)(fd, buf, count, offset);
	}

	if (!EVENT_IS_IO_URING(fiber_io_event())) {
		return (*sys_pread)(fd, buf, count, offset);
	}

	// We alloc one new FILE_EVENT for the fd so that multiple fibers
	// can pread or pwrite the same fd.

	FILE_ALLOC(fe, EVENT_READ);
	fe->fd = fd;
	fe->in.read_ctx.off = offset;
	ret = fiber_iocp_read(fe, buf, (int) count);
	file_event_unrefer(fe);

	return ret;
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset)
{
	FILE_EVENT *fe;
	ssize_t ret;

	if (fd <= INVALID_SOCKET) {
		msg_error("%s: invalid fd: %d", __FUNCTION__, fd);
		return -1;
	}

	if (sys_pwrite == NULL) {
		hook_once();
	}

	if (!var_hook_sys_api) {
		return (*sys_pwrite)(fd, buf, count, offset);
	}

	if (!EVENT_IS_IO_URING(fiber_io_event())) {
		return (*sys_pwrite)(fd, buf, count, offset);
	}

	FILE_ALLOC(fe, EVENT_WRITE);
	fe->fd = fd;
	fe->out.write_ctx.off = offset;
	ret = fiber_iocp_write(fe, buf, (int) count);
	file_event_unrefer(fe);

	return ret;
}

ssize_t splice(int fd_in, loff_t *poff_in, int fd_out,
	loff_t *poff_out, size_t len, unsigned int flags)
{
	FILE_EVENT *fe;
	EVENT *ev;
	int    ret;
	loff_t off_in, off_out;
	unsigned int sqe_flags = 0;

	if (fd_in < 0 || fd_out < 0) {
		msg_error("%s: invalid fd_in: %d, fd_out: %d",
			__FUNCTION__, fd_in, fd_out);
		return -1;
	}

	CHECK_API("sys_splice", sys_splice);

	if (!var_hook_sys_api) {
		return (*sys_splice)(fd_in, poff_in, fd_out, poff_out, len, flags);
	}

	ev = fiber_io_event();

	if (!EVENT_IS_IO_URING(ev)) {
		return (*sys_splice)(fd_in, poff_in, fd_out, poff_out, len, flags);
	}

	off_in  = poff_in  ? *poff_in : -1;
	off_out = poff_out ? *poff_out : -1;

	// The same fd_in maybe be shared by multiple fibers, so we should
	// alloc one new FILE_EVENT for each operation.
	FILE_ALLOC(fe, EVENT_SPLICE);
	fe->fiber_r->status = FIBER_STATUS_WAIT_READ;

	event_uring_splice(ev, fe, fd_in, off_in, fd_out, off_out, len, flags,
		sqe_flags, IORING_OP_SPLICE);

	ev->waiter++;
	acl_fiber_switch();
	ev->waiter--;

	fe->mask &= ~EVENT_SPLICE;

	if (fe->reader_ctx.res < 0) {
		acl_fiber_set_error(-fe->reader_ctx.res);
		file_event_unrefer(fe);
		return -1;
	}
	
	if (off_in != -1 && poff_in) {
		*poff_in += fe->reader_ctx.res;
	}

	if (off_out != -1 && poff_out) {
		*poff_out += fe->reader_ctx.res;
	}

	ret = fe->reader_ctx.res;
	file_event_unrefer(fe);
	return ret;
}

ssize_t file_sendfile(socket_t out_fd, int in_fd, off64_t *off, size_t cnt)
{
	unsigned flags = SPLICE_F_MOVE | SPLICE_F_MORE | SPLICE_F_NONBLOCK;
	ssize_t ret;
	int pipefd[2];


	if (pipe(pipefd) == -1) {
		msg_error("%s(%d): pipe error=%s",
			__FUNCTION__, __LINE__, last_serror());
		return -1;
	}

	ret = splice(in_fd, off, pipefd[1], NULL, cnt, flags);
	if (ret <= 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return ret;
	}

	ret = splice(pipefd[0], NULL, out_fd, NULL, ret, flags);
	if (ret <= 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return ret;
	}

	close(pipefd[0]);
	close(pipefd[1]);
	return ret;
}

#endif // HAS_IO_URING
