#ifndef _OVL_FS_COMPAT_H_
#define _OVL_FS_COMPAT_H_

#define COMPAT_WORKDIR "..overlayfs.compat.workdir.do.not.touch"

#ifdef CONFIG_OVERLAY_FS_COMPAT
bool ovl_compat_mode(struct dentry *dentry);
int ovl_compat_whiteout(struct dentry *workdir, struct dentry *dentry,
			struct dentry *whiteout);
bool ovl_compat_is_whiteout(struct dentry *ovl_dir, struct dentry *dentry);
bool ovl_compat_maybe_whiteout(struct dentry *dentry, char d_type, ino_t ino);

int ovl_compat_mkdir(const char *name, struct path *path);

#else
static inline bool ovl_compat_mode(struct dentry *dentry)
{
	return false;
}

static inline int ovl_compat_whiteout(struct dentry *workdir,
				      struct dentry *dentry,
				      struct dentry *whiteout)
{
	return -EINVAL;
}

static inline bool ovl_compat_is_whiteout(struct dentry *ovl_dir,
					  struct dentry *dentry)
{
	return false;
}

static inline bool ovl_compat_maybe_whiteout(struct dentry *dentry,
					     char d_type, ino_t ino)
{
	return false;
}

static inline int ovl_compat_mkdir(const char *name, struct path *path)
{
	return 0;
}
#endif
#endif /* _OVL_FS_COMPAT_H_ */
