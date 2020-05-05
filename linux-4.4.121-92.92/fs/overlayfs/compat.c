#include <linux/fs.h>
#include <linux/xattr.h>
#include <linux/cred.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/module.h>
#include "overlayfs.h"
#include "compat.h"

static const char *ovl_whiteout_symlink = "(overlay-whiteout)";
static const char *ovl_whiteout_xattr = "trusted.overlay.whiteout";

int ovl_compat_whiteout(struct dentry *workdir, struct dentry *dentry,
			struct dentry *whiteout)
{
        int err;
        struct dentry *newdentry;
        const struct cred *old_cred;
        struct cred *override_cred;

        /* FIXME: recheck lower dentry to see if whiteout is really needed */

        err = -ENOMEM;
        override_cred = prepare_creds();
        if (!override_cred)
                goto out;

        /*
         * CAP_SYS_ADMIN for setxattr
         * CAP_DAC_OVERRIDE for symlink creation
         * CAP_FOWNER for unlink in sticky directory
         */
        cap_raise(override_cred->cap_effective, CAP_SYS_ADMIN);
        cap_raise(override_cred->cap_effective, CAP_DAC_OVERRIDE);
        cap_raise(override_cred->cap_effective, CAP_FOWNER);
        override_cred->fsuid = GLOBAL_ROOT_UID;
        override_cred->fsgid = GLOBAL_ROOT_GID;
        old_cred = override_creds(override_cred);

        newdentry = lookup_one_len(whiteout->d_name.name, workdir,
                                   whiteout->d_name.len);
        err = PTR_ERR(newdentry);
        if (IS_ERR(newdentry))
                goto out_put_cred;
        if (IS_ERR(newdentry))
                goto out_put_cred;

        /* Just been removed within the same locked region */
        WARN_ON(newdentry->d_inode);

        err = vfs_symlink(workdir->d_inode, newdentry, ovl_whiteout_symlink);
        if (err)
                goto out_dput;

        ovl_dentry_version_inc(dentry->d_parent);

        err = vfs_setxattr(newdentry, ovl_whiteout_xattr, "y", 1, 0);
        if (err)
                vfs_unlink(workdir->d_inode, newdentry, NULL);

out_dput:
        dput(newdentry);
out_put_cred:
        revert_creds(old_cred);
        put_cred(override_cred);
out:
        if (err) {
                /*
                 * There's no way to recover from failure to whiteout.
                 * What should we do?  Log a big fat error and... ?
                 */
                pr_err("overlayfs: ERROR - failed to whiteout '%s'\n",
                       dentry->d_name.name);
        }

        return err;
}

bool ovl_compat_maybe_whiteout(struct dentry *dentry, char d_type, ino_t ino)
{
	struct dentry *workdir = ovl_workdir(dentry);

	if (!ovl_compat_mode(dentry))
		return false;

	if (d_type == DT_LNK)
		return true;

	return workdir->d_parent->d_inode->i_ino == ino;
}

bool ovl_compat_is_whiteout(struct dentry *ovl_dir, struct dentry *dentry)
{
	int res;
	char val;

	if (!ovl_compat_mode(ovl_dir))
		return false;

	if (!dentry)
		return false;

	if (!dentry->d_inode)
		return false;

	/* Hide the workdir we created */
	if (dentry == ovl_workdir(ovl_dir)->d_parent)
		return true;

	if (!S_ISLNK(dentry->d_inode->i_mode))
		return false;

	res = vfs_getxattr(dentry, ovl_whiteout_xattr, &val, 1);
	if (res == 1 && val == 'y')
		return true;

	return false;
}

int ovl_compat_mkdir(const char *name, struct path *path)
{
	struct dentry *dentry;
	struct path tmppath;
	int err;

	dentry = kern_path_create(AT_FDCWD, name, &tmppath,
				  LOOKUP_FOLLOW|LOOKUP_DIRECTORY);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);

	err = vfs_mkdir(tmppath.dentry->d_inode, dentry, 0|S_IFDIR);
	if (!err) {
		path->mnt = mntget(tmppath.mnt);
		path->dentry = dget(dentry);
	}
	done_path_create(&tmppath, dentry);

	return err;
}
