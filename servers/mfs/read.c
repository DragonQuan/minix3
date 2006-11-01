

#include "fs.h"
#include <fcntl.h>
#include <unistd.h>
#include <minix/com.h>
#include "buf.h"
#include "inode.h"
#include "super.h"

#include <minix/vfsif.h>



FORWARD _PROTOTYPE( int rw_chunk, (struct inode *rip, off_t position,
	unsigned off, int chunk, unsigned left, int rw_flag,
	char *buff, int seg, int usr, int block_size, int *completed));


/*===========================================================================*
 *				fs_readwrite				     *
 *===========================================================================*/
PUBLIC int fs_readwrite(void)
{
  int r, usr, seg, rw_flag, chunk, block_size, block_spec;
  int partial_cnt, regular, partial_pipe, nrbytes;
  off_t position, f_size, bytes_left;
  unsigned int off, cum_io;
  mode_t mode_word;
  int completed, r2 = OK;
  char *user_addr;
  struct inode *rip;
  
  partial_pipe = 0;
  r = OK;
  
  /* Try to get inode according to its index */
  if (fs_m_in.REQ_FD_INODE_INDEX >= 0 && 
          fs_m_in.REQ_FD_INODE_INDEX < NR_INODES &&
          inode[fs_m_in.REQ_FD_INODE_INDEX].i_num == fs_m_in.REQ_FD_INODE_NR) {
      rip = &inode[fs_m_in.REQ_FD_INODE_INDEX];
  }
  else { 
      /* Find the inode referred */
      rip = find_inode(fs_dev, fs_m_in.REQ_FD_INODE_NR);
      if (!rip) {
          printf("FS: unavaliable inode by fs_readwrite(), nr: %d\n", 
                  fs_m_in.REQ_FD_INODE_NR);
          return EINVAL; 
      }
  }

  mode_word = rip->i_mode & I_TYPE;
  regular = (mode_word == I_REGULAR || mode_word == I_NAMED_PIPE);
  block_spec = (mode_word == I_BLOCK_SPECIAL ? 1 : 0);
  
  /* Determine blocksize */
  block_size = (block_spec ? get_block_size(rip->i_zone[0]) 
      : rip->i_sp->s_block_size);

  f_size = (block_spec ? ULONG_MAX : rip->i_size);
  
  /* Get the values from the request message */ 
  rw_flag = (fs_m_in.m_type == REQ_READ ? READING : WRITING);
  usr = fs_m_in.REQ_FD_WHO_E;
  seg = fs_m_in.REQ_FD_SEG;
  position = fs_m_in.REQ_FD_POS;
  nrbytes = (unsigned) fs_m_in.REQ_FD_NBYTES;
  /*partial_cnt = fs_m_in.REQ_FD_PARTIAL;*/
  user_addr = fs_m_in.REQ_FD_USER_ADDR;

  /*if (partial_cnt > 0) partial_pipe = 1;*/
  
  rdwt_err = OK;		/* set to EIO if disk error occurs */
  
  if (rw_flag == WRITING && block_spec == 0) {
      /* Clear the zone containing present EOF if hole about
       * to be created.  This is necessary because all unwritten
       * blocks prior to the EOF must read as zeros.
       */
      if (position > f_size) clear_zone(rip, f_size, 0);
  }
	      
  cum_io = 0;
  /* Split the transfer into chunks that don't span two blocks. */
  while (nrbytes != 0) {
      off = (unsigned int) (position % block_size);/* offset in blk*/
          
      chunk = MIN(nrbytes, block_size - off);
      if (chunk < 0) chunk = block_size - off;

      if (rw_flag == READING) {
          bytes_left = f_size - position;
          if (position >= f_size) break;	/* we are beyond EOF */
          if (chunk > bytes_left) chunk = (int) bytes_left;
      }

      /* Read or write 'chunk' bytes. */
      r = rw_chunk(rip, position, off, chunk, (unsigned) nrbytes,
              rw_flag, user_addr, seg, usr, block_size, &completed);

      if (r != OK) break;	/* EOF reached */
      if (rdwt_err < 0) break;

      /* Update counters and pointers. */
      user_addr += chunk;	/* user buffer address */
      nrbytes -= chunk;	/* bytes yet to be read */
      cum_io += chunk;	/* bytes read so far */
      position += chunk;	/* position within the file */
  }

  fs_m_out.RES_FD_POS = position; /* It might change later and the VFS has
				     to know this value */
  
  /* On write, update file size and access time. */
  if (rw_flag == WRITING) {
	if (regular || mode_word == I_DIRECTORY) {
		if (position > f_size) rip->i_size = position;
	}
  } 
  else {
	if (rip->i_pipe == I_PIPE) {
		if ( position >= rip->i_size) {
			/* Reset pipe pointers. */
			rip->i_size = 0;	/* no data left */
			position = 0;		/* reset reader(s) */
		}
	}
  }

  /* Check to see if read-ahead is called for, and if so, set it up. */
  if (rw_flag == READING && rip->i_seek == NO_SEEK && position % block_size == 0
		&& (regular || mode_word == I_DIRECTORY)) {
	rdahed_inode = rip;
	rdahedpos = position;
  }
  rip->i_seek = NO_SEEK;

  if (rdwt_err != OK) r = rdwt_err;	/* check for disk error */
  if (rdwt_err == END_OF_FILE) r = OK;

  /* if user-space copying failed, read/write failed. */
  if (r == OK && r2 != OK) {
	r = r2;
  }
  
  if (r == OK) {
	if (rw_flag == READING) rip->i_update |= ATIME;
	if (rw_flag == WRITING) rip->i_update |= CTIME | MTIME;
	rip->i_dirt = DIRTY;		/* inode is thus now dirty */
  }
  
  fs_m_out.RES_FD_CUM_IO = cum_io;
  fs_m_out.RES_FD_SIZE = rip->i_size;
  
  return(r);
}


/*===========================================================================*
 *				fs_breadwrite				     *
 *===========================================================================*/
PUBLIC int fs_breadwrite(void)
{
  int r, usr, rw_flag, chunk, block_size;
  int nrbytes;
  off_t position, f_size, bytes_left;
  unsigned int off, cum_io;
  mode_t mode_word;
  int completed, r2 = OK;
  char *user_addr;

  /* Pseudo inode for rw_chunk */
  struct inode rip;
  
  r = OK;
  f_size = ULONG_MAX;
  
  /* Get the values from the request message */ 
  rw_flag = (fs_m_in.m_type == REQ_BREAD ? READING : WRITING);
  usr = fs_m_in.REQ_FD_WHO_E;
  position = fs_m_in.REQ_FD_POS;
  nrbytes = (unsigned) fs_m_in.REQ_FD_NBYTES;
  user_addr = fs_m_in.REQ_FD_USER_ADDR;
  
  block_size = get_block_size(fs_m_in.REQ_FD_BDEV);

  rip.i_zone[0] = fs_m_in.REQ_FD_BDEV;
  rip.i_mode = I_BLOCK_SPECIAL;
  rip.i_size = f_size;

  rdwt_err = OK;		/* set to EIO if disk error occurs */
  
  cum_io = 0;
  /* Split the transfer into chunks that don't span two blocks. */
  while (nrbytes != 0) {
      off = (unsigned int) (position % block_size);/* offset in blk*/
        
      chunk = MIN(nrbytes, block_size - off);
      if (chunk < 0) chunk = block_size - off;

      if (rw_flag == READING) {
          bytes_left = f_size - position;
          if (position >= f_size) break;	/* we are beyond EOF */
          if (chunk > bytes_left) chunk = (int) bytes_left;
      }

      /* Read or write 'chunk' bytes. */
      r = rw_chunk(&rip, position, off, chunk, (unsigned) nrbytes,
              rw_flag, user_addr, D, usr, block_size, &completed);

      if (r != OK) break;	/* EOF reached */
      if (rdwt_err < 0) break;

      /* Update counters and pointers. */
      user_addr += chunk;	/* user buffer address */
      nrbytes -= chunk;	        /* bytes yet to be read */
      cum_io += chunk;	        /* bytes read so far */
      position += chunk;	/* position within the file */
  }
  
  fs_m_out.RES_FD_POS = position; 
  
  if (rdwt_err != OK) r = rdwt_err;	/* check for disk error */
  if (rdwt_err == END_OF_FILE) r = OK;

  fs_m_out.RES_FD_CUM_IO = cum_io;
  fs_m_out.RES_FD_SIZE = rip.i_size;
  
  return(r);
}


/*===========================================================================*
 *				rw_chunk				     *
 *===========================================================================*/
PRIVATE int rw_chunk(rip, position, off, chunk, left, rw_flag, buff,
 seg, usr, block_size, completed)
register struct inode *rip;	/* pointer to inode for file to be rd/wr */
off_t position;			/* position within file to read or write */
unsigned off;			/* off within the current block */
int chunk;			/* number of bytes to read or write */
unsigned left;			/* max number of bytes wanted after position */
int rw_flag;			/* READING or WRITING */
char *buff;			/* virtual address of the user buffer */
int seg;			/* T or D segment in user space */
int usr;			/* which user process */
int block_size;			/* block size of FS operating on */
int *completed;			/* number of bytes copied */
{
/* Read or write (part of) a block. */

  register struct buf *bp;
  register int r = OK;
  int n, block_spec;
  block_t b;
  dev_t dev;

  *completed = 0;

  block_spec = (rip->i_mode & I_TYPE) == I_BLOCK_SPECIAL;

  if (block_spec) {
	b = position/block_size;
	dev = (dev_t) rip->i_zone[0];
  } 
  else {
	b = read_map(rip, position);
	dev = rip->i_dev;
  }

  if (!block_spec && b == NO_BLOCK) {
	if (rw_flag == READING) {
		/* Reading from a nonexistent block.  Must read as all zeros.*/
		bp = get_block(NO_DEV, NO_BLOCK, NORMAL);    /* get a buffer */
		zero_block(bp);
	} 
        else {
		/* Writing to a nonexistent block. Create and enter in inode.*/
		if ((bp= new_block(rip, position)) == NIL_BUF)return(err_code);
	}
  } 
  else if (rw_flag == READING) {
	/* Read and read ahead if convenient. */
	bp = rahead(rip, b, position, left);
  } 
  else {
	/* Normally an existing block to be partially overwritten is first read
	 * in.  However, a full block need not be read in.  If it is already in
	 * the cache, acquire it, otherwise just acquire a free buffer.
	 */
	n = (chunk == block_size ? NO_READ : NORMAL);
	if (!block_spec && off == 0 && position >= rip->i_size) n = NO_READ;
	bp = get_block(dev, b, n);
  }

  /* In all cases, bp now points to a valid buffer. */
  if (bp == NIL_BUF) {
  	panic(__FILE__,"bp not valid in rw_chunk, this can't happen", NO_NUM);
  }
  
  if (rw_flag == WRITING && chunk != block_size && !block_spec &&
					position >= rip->i_size && off == 0) {
	zero_block(bp);
  }

  if (rw_flag == READING) {
	/* Copy a chunk from the block buffer to user space. */
	r = sys_vircopy(SELF_E, D, (phys_bytes) (bp->b_data+off),
			usr, seg, (phys_bytes) buff,
			(phys_bytes) chunk);
  } 
  else {
	/* Copy a chunk from user space to the block buffer. */
	r = sys_vircopy(usr, seg, (phys_bytes) buff,
			SELF_E, D, (phys_bytes) (bp->b_data+off),
			(phys_bytes) chunk);
	bp->b_dirt = DIRTY;
  }
  n = (off + chunk == block_size ? FULL_DATA_BLOCK : PARTIAL_DATA_BLOCK);
  put_block(bp, n);

  return(r);
}


/*===========================================================================*
 *				read_map				     *
 *===========================================================================*/
PUBLIC block_t read_map(rip, position)
register struct inode *rip;	/* ptr to inode to map from */
off_t position;			/* position in file whose blk wanted */
{
/* Given an inode and a position within the corresponding file, locate the
 * block (not zone) number in which that position is to be found and return it.
 */

  register struct buf *bp;
  register zone_t z;
  int scale, boff, dzones, nr_indirects, index, zind, ex;
  block_t b;
  long excess, zone, block_pos;
  
  scale = rip->i_sp->s_log_zone_size;	/* for block-zone conversion */
  block_pos = position/rip->i_sp->s_block_size;	/* relative blk # in file */
  zone = block_pos >> scale;	/* position's zone */
  boff = (int) (block_pos - (zone << scale) ); /* relative blk # within zone */
  dzones = rip->i_ndzones;
  nr_indirects = rip->i_nindirs;

  /* Is 'position' to be found in the inode itself? */
  if (zone < dzones) {
	zind = (int) zone;	/* index should be an int */
	z = rip->i_zone[zind];
	if (z == NO_ZONE) return(NO_BLOCK);
	b = ((block_t) z << scale) + boff;
	return(b);
  }

  /* It is not in the inode, so it must be single or double indirect. */
  excess = zone - dzones;	/* first Vx_NR_DZONES don't count */

  if (excess < nr_indirects) {
	/* 'position' can be located via the single indirect block. */
	z = rip->i_zone[dzones];
  } else {
	/* 'position' can be located via the double indirect block. */
	if ( (z = rip->i_zone[dzones+1]) == NO_ZONE) return(NO_BLOCK);
	excess -= nr_indirects;			/* single indir doesn't count*/
	b = (block_t) z << scale;
	bp = get_block(rip->i_dev, b, NORMAL);	/* get double indirect block */
	index = (int) (excess/nr_indirects);
	z = rd_indir(bp, index);		/* z= zone for single*/
	put_block(bp, INDIRECT_BLOCK);		/* release double ind block */
	excess = excess % nr_indirects;		/* index into single ind blk */
  }

  /* 'z' is zone num for single indirect block; 'excess' is index into it. */
  if (z == NO_ZONE) return(NO_BLOCK);
  b = (block_t) z << scale;			/* b is blk # for single ind */
  bp = get_block(rip->i_dev, b, NORMAL);	/* get single indirect block */
  ex = (int) excess;				/* need an integer */
  z = rd_indir(bp, ex);				/* get block pointed to */
  put_block(bp, INDIRECT_BLOCK);		/* release single indir blk */
  if (z == NO_ZONE) return(NO_BLOCK);
  b = ((block_t) z << scale) + boff;
  return(b);
}

/*===========================================================================*
 *				rd_indir				     *
 *===========================================================================*/
PUBLIC zone_t rd_indir(bp, index)
struct buf *bp;			/* pointer to indirect block */
int index;			/* index into *bp */
{
/* Given a pointer to an indirect block, read one entry.  The reason for
 * making a separate routine out of this is that there are four cases:
 * V1 (IBM and 68000), and V2 (IBM and 68000).
 */

  struct super_block *sp;
  zone_t zone;			/* V2 zones are longs (shorts in V1) */

  if(bp == NIL_BUF)
	panic(__FILE__, "rd_indir() on NIL_BUF", NO_NUM);

  sp = get_super(bp->b_dev);	/* need super block to find file sys type */

  /* read a zone from an indirect block */
  if (sp->s_version == V1)
	zone = (zone_t) conv2(sp->s_native, (int)  bp->b_v1_ind[index]);
  else
	zone = (zone_t) conv4(sp->s_native, (long) bp->b_v2_ind[index]);

  if (zone != NO_ZONE &&
		(zone < (zone_t) sp->s_firstdatazone || zone >= sp->s_zones)) {
	printf("Illegal zone number %ld in indirect block, index %d\n",
	       (long) zone, index);
	panic(__FILE__,"check file system", NO_NUM);
  }
  return(zone);
}

/*===========================================================================*
 *				read_ahead				     *
 *===========================================================================*/
PUBLIC void read_ahead()
{
/* Read a block into the cache before it is needed. */
  int block_size;
  register struct inode *rip;
  struct buf *bp;
  block_t b;

  rip = rdahed_inode;		/* pointer to inode to read ahead from */
  block_size = get_block_size(rip->i_dev);
  rdahed_inode = NIL_INODE;	/* turn off read ahead */
  if ( (b = read_map(rip, rdahedpos)) == NO_BLOCK) return;	/* at EOF */
  bp = rahead(rip, b, rdahedpos, block_size);
  put_block(bp, PARTIAL_DATA_BLOCK);
}

/*===========================================================================*
 *				rahead					     *
 *===========================================================================*/
PUBLIC struct buf *rahead(rip, baseblock, position, bytes_ahead)
register struct inode *rip;	/* pointer to inode for file to be read */
block_t baseblock;		/* block at current position */
off_t position;			/* position within file */
unsigned bytes_ahead;		/* bytes beyond position for immediate use */
{
/* Fetch a block from the cache or the device.  If a physical read is
 * required, prefetch as many more blocks as convenient into the cache.
 * This usually covers bytes_ahead and is at least BLOCKS_MINIMUM.
 * The device driver may decide it knows better and stop reading at a
 * cylinder boundary (or after an error).  Rw_scattered() puts an optional
 * flag on all reads to allow this.
 */
  int block_size;
/* Minimum number of blocks to prefetch. */
# define BLOCKS_MINIMUM		(NR_BUFS < 50 ? 18 : 32)
  int block_spec, scale, read_q_size;
  unsigned int blocks_ahead, fragment;
  block_t block, blocks_left;
  off_t ind1_pos;
  dev_t dev;
  struct buf *bp;
  static struct buf *read_q[NR_BUFS];

  block_spec = (rip->i_mode & I_TYPE) == I_BLOCK_SPECIAL;
  if (block_spec) {
	dev = (dev_t) rip->i_zone[0];
  } else {
	dev = rip->i_dev;
  }
  block_size = get_block_size(dev);

  block = baseblock;
  bp = get_block(dev, block, PREFETCH);
  if (bp->b_dev != NO_DEV) return(bp);

  /* The best guess for the number of blocks to prefetch:  A lot.
   * It is impossible to tell what the device looks like, so we don't even
   * try to guess the geometry, but leave it to the driver.
   *
   * The floppy driver can read a full track with no rotational delay, and it
   * avoids reading partial tracks if it can, so handing it enough buffers to
   * read two tracks is perfect.  (Two, because some diskette types have
   * an odd number of sectors per track, so a block may span tracks.)
   *
   * The disk drivers don't try to be smart.  With todays disks it is
   * impossible to tell what the real geometry looks like, so it is best to
   * read as much as you can.  With luck the caching on the drive allows
   * for a little time to start the next read.
   *
   * The current solution below is a bit of a hack, it just reads blocks from
   * the current file position hoping that more of the file can be found.  A
   * better solution must look at the already available zone pointers and
   * indirect blocks (but don't call read_map!).
   */

  fragment = position % block_size;
  position -= fragment;
  bytes_ahead += fragment;

  blocks_ahead = (bytes_ahead + block_size - 1) / block_size;

  if (block_spec && rip->i_size == 0) {
	blocks_left = NR_IOREQS;
  } else {
	blocks_left = (rip->i_size - position + block_size - 1) / block_size;

	/* Go for the first indirect block if we are in its neighborhood. */
	if (!block_spec) {
		scale = rip->i_sp->s_log_zone_size;
		ind1_pos = (off_t) rip->i_ndzones * (block_size << scale);
		if (position <= ind1_pos && rip->i_size > ind1_pos) {
			blocks_ahead++;
			blocks_left++;
		}
	}
  }

  /* No more than the maximum request. */
  if (blocks_ahead > NR_IOREQS) blocks_ahead = NR_IOREQS;

  /* Read at least the minimum number of blocks, but not after a seek. */
  if (blocks_ahead < BLOCKS_MINIMUM && rip->i_seek == NO_SEEK)
	blocks_ahead = BLOCKS_MINIMUM;

  /* Can't go past end of file. */
  if (blocks_ahead > blocks_left) blocks_ahead = blocks_left;

  read_q_size = 0;

  /* Acquire block buffers. */
  for (;;) {
	read_q[read_q_size++] = bp;

	if (--blocks_ahead == 0) break;

	/* Don't trash the cache, leave 4 free. */
	if (bufs_in_use >= NR_BUFS - 4) break;

	block++;

	bp = get_block(dev, block, PREFETCH);
	if (bp->b_dev != NO_DEV) {
		/* Oops, block already in the cache, get out. */
		put_block(bp, FULL_DATA_BLOCK);
		break;
	}
  }
  rw_scattered(dev, read_q, read_q_size, READING);
  return(get_block(dev, baseblock, NORMAL));
}
