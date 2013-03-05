#include <unistd.h>

#undef LOG_PREFIX
#define LOG_PREFIX "page-pipe: "

#include "util.h"
#include "page-pipe.h"

static int page_pipe_grow(struct page_pipe *pp)
{
	struct page_pipe_buf *ppb;

	pr_debug("Will grow page pipe (iov off is %u)\n", pp->free_iov);

	ppb = xmalloc(sizeof(*ppb));
	if (!ppb)
		return -1;

	if (pipe(ppb->p)) {
		pr_perror("Can't make pipe for page-pipe");
		return -1;
	}

	ppb->pipe_size = fcntl(ppb->p[0], F_GETPIPE_SZ, 0) / PAGE_SIZE;
	ppb->pages_in = 0;
	ppb->nr_segs = 0;
	ppb->iov = &pp->iovs[pp->free_iov];

	list_add_tail(&ppb->l, &pp->bufs);
	pp->nr_pipes++;

	return 0;
}

struct page_pipe *create_page_pipe(unsigned int nr_segs, struct iovec *iovs)
{
	struct page_pipe *pp;

	pr_debug("Create page pipe for %u segs\n", nr_segs);

	pp = xmalloc(sizeof(*pp));
	if (pp) {
		pp->nr_pipes = 0;
		INIT_LIST_HEAD(&pp->bufs);
		pp->nr_iovs = nr_segs;
		pp->iovs = iovs;
		pp->free_iov = 0;

		if (page_pipe_grow(pp))
			return NULL;
	}

	return pp;
}

void destroy_page_pipe(struct page_pipe *pp)
{
	struct page_pipe_buf *ppb, *n;

	pr_debug("Killing page pipe\n");

	list_for_each_entry_safe(ppb, n, &pp->bufs, l) {
		close(ppb->p[0]);
		close(ppb->p[1]);
	}

	xfree(pp);
}

#define PPB_IOV_BATCH	8

static inline int try_add_page_to(struct page_pipe *pp, struct page_pipe_buf *ppb,
		unsigned long addr)
{
	struct iovec *iov;

	if (ppb->pages_in == ppb->pipe_size) {
		int ret;

		ret = fcntl(ppb->p[0], F_SETPIPE_SZ, (ppb->pipe_size * PAGE_SIZE) << 1);
		if (ret < 0)
			return 1; /* need to add another buf */

		ret /= PAGE_SIZE;
		BUG_ON(ret < ppb->pipe_size);

		pr_debug("Grow pipe %x -> %x\n", ppb->pipe_size, ret);
		ppb->pipe_size = ret;
	}

	if (ppb->nr_segs) {
		/* can existing iov accumulate the page? */
		iov = &ppb->iov[ppb->nr_segs - 1];
		if ((unsigned long)iov->iov_base + iov->iov_len == addr) {
			iov->iov_len += PAGE_SIZE;
			goto out;
		}

		if (ppb->nr_segs == UIO_MAXIOV)
			/* XXX -- shink pipe back? */
			return 1;
	}

	pr_debug("Add iov to page pipe (%u iovs, %u/%u total)\n",
			ppb->nr_segs, pp->free_iov, pp->nr_iovs);
	ppb->iov[ppb->nr_segs].iov_base = (void *)addr;
	ppb->iov[ppb->nr_segs].iov_len = PAGE_SIZE;
	ppb->nr_segs++;
	pp->free_iov++;
	BUG_ON(pp->free_iov > pp->nr_iovs);
out:
	ppb->pages_in++;
	return 0;
}

static inline int try_add_page(struct page_pipe *pp, unsigned long addr)
{
	BUG_ON(list_empty(&pp->bufs));
	return try_add_page_to(pp, list_entry(pp->bufs.prev, struct page_pipe_buf, l), addr);
}

int page_pipe_add_page(struct page_pipe *pp, unsigned long addr)
{
	int ret;

	ret = try_add_page(pp, addr);
	if (ret <= 0)
		return ret;

	ret = page_pipe_grow(pp);
	if (ret < 0)
		return ret;

	ret = try_add_page(pp, addr);
	BUG_ON(ret > 0);
	return ret;
}

static int page_buf_iterate(struct page_pipe_buf *ppb,
		int (*fn)(int rpipe, unsigned long addr, void *), void *a)
{
	unsigned int pg;
	unsigned long addr, seg_end;
	struct iovec *iov;

	pr_debug("Iterate ppb of %u pages, %u segs\n", ppb->pages_in, ppb->nr_segs);

	iov = &ppb->iov[0];
	addr = (unsigned long)iov->iov_base;
	seg_end = addr + iov->iov_len;

	for (pg = 0; pg < ppb->pages_in; pg++) {
		int ret;

		if (addr >= seg_end) {
			iov++;
			BUG_ON(iov - ppb->iov >= ppb->nr_segs);
			addr = (unsigned long)iov->iov_base;
			seg_end = addr + iov->iov_len;
		}

		ret = fn(ppb->p[0], addr, a);
		if (ret)
			return ret;

		addr += PAGE_SIZE;
	}

	return 0;
}

int page_pipe_iterate_pages(struct page_pipe *pp,
		int (*fn)(int rpipe, unsigned long addr, void *), void *a)
{
	int ret = 0;
	struct page_pipe_buf *ppb;

	pr_debug("Iterate pp\n");

	list_for_each_entry(ppb, &pp->bufs, l) {
		ret = page_buf_iterate(ppb, fn, a);
		if (ret)
			break;
	}

	pr_debug("Done iteration\n");

	return ret;
}