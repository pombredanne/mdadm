/*
 * mdadm - manage Linux "md" devices aka RAID arrays.
 *
 * Copyright (C) 2001-2009 Neil Brown <neilb@suse.de>
 *
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *    Author: Neil Brown
 *    Email: <neilb@suse.de>
 */

#include	"mdadm.h"
#include	"md_p.h"
#include	"md_u.h"
#include	<sys/wait.h>
#include	<signal.h>
#include	<limits.h>
#include	<syslog.h>

/* The largest number of disks current arrays can manage is 384
 * This really should be dynamically, but that will have to wait
 * At least it isn't MD_SB_DISKS.
 */
#define MaxDisks 384
struct state {
	char *devname;
	int devnum;	/* to sync with mdstat info */
	long utime;
	int err;
	char *spare_group;
	int active, working, failed, spare, raid;
	int expected_spares;
	int devstate[MaxDisks];
	unsigned devid[MaxDisks];
	int percent;
	int parent_dev; /* For subarray, devnum of parent.
			 * For others, NoMdDev
			 */
	struct supertype *metadata;
	struct state *subarray;/* for a container it is a link to first subarray
				* for a subarray it is a link to next subarray
				* in the same container */
	struct state *parent;  /* for a subarray it is a link to its container
				*/
	struct state *next;
};

struct alert_info {
	char *mailaddr;
	char *mailfrom;
	char *alert_cmd;
	int dosyslog;
};
static int make_daemon(char *pidfile);
static int check_one_sharer(int scan);
static void alert(char *event, char *dev, char *disc, struct alert_info *info);
static int check_array(struct state *st, struct mdstat_ent *mdstat,
		       int test, struct alert_info *info,
		       int increments);
static int add_new_arrays(struct mdstat_ent *mdstat, struct state *statelist,
			  int test, struct alert_info *info);
static void try_spare_migration(struct state *statelist, struct alert_info *info);
static void link_containers_with_subarrays(struct state *list);

int Monitor(struct mddev_dev *devlist,
	    char *mailaddr, char *alert_cmd,
	    int period, int daemonise, int scan, int oneshot,
	    int dosyslog, int test, char *pidfile, int increments,
	    int share)
{
	/*
	 * Every few seconds, scan every md device looking for changes
	 * When a change is found, log it, possibly run the alert command,
	 * and possibly send Email
	 *
	 * For each array, we record:
	 *   Update time
	 *   active/working/failed/spare drives
	 *   State of each device.
	 *   %rebuilt if rebuilding
	 *
	 * If the update time changes, check out all the data again
	 * It is possible that we cannot get the state of each device
	 * due to bugs in the md kernel module.
	 * We also read /proc/mdstat to get rebuild percent,
	 * and to get state on all active devices incase of kernel bug.
	 *
	 * Events are:
	 *    Fail
	 *	An active device had Faulty set or Active/Sync removed
	 *    FailSpare
	 *      A spare device had Faulty set
	 *    SpareActive
	 *      An active device had a reverse transition
	 *    RebuildStarted
	 *      percent went from -1 to +ve
	 *    RebuildNN
	 *      percent went from below to not-below NN%
	 *    DeviceDisappeared
	 *      Couldn't access a device which was previously visible
	 *
	 * if we detect an array with active<raid and spare==0
	 * we look at other arrays that have same spare-group
	 * If we find one with active==raid and spare>0,
	 *  and if we can get_disk_info and find a name
	 *  Then we hot-remove and hot-add to the other array
	 *
	 * If devlist is NULL, then we can monitor everything because --scan
	 * was given.  We get an initial list from config file and add anything
	 * that appears in /proc/mdstat
	 */

	struct state *statelist = NULL;
	int finished = 0;
	struct mdstat_ent *mdstat = NULL;
	char *mailfrom = NULL;
	struct alert_info info;

	if (!mailaddr) {
		mailaddr = conf_get_mailaddr();
		if (mailaddr && ! scan)
			fprintf(stderr, Name ": Monitor using email address \"%s\" from config file\n",
			       mailaddr);
	}
	mailfrom = conf_get_mailfrom();

	if (!alert_cmd) {
		alert_cmd = conf_get_program();
		if (alert_cmd && ! scan)
			fprintf(stderr, Name ": Monitor using program \"%s\" from config file\n",
			       alert_cmd);
	}
	if (scan && !mailaddr && !alert_cmd) {
		fprintf(stderr, Name ": No mail address or alert command - not monitoring.\n");
		return 1;
	}
	info.alert_cmd = alert_cmd;
	info.mailaddr = mailaddr;
	info.mailfrom = mailfrom;
	info.dosyslog = dosyslog;

	if (daemonise)
		if (make_daemon(pidfile))
			return 1;

	if (share) 
		if (check_one_sharer(scan))
			return 1;

	if (devlist == NULL) {
		struct mddev_ident *mdlist = conf_get_ident(NULL);
		for (; mdlist; mdlist=mdlist->next) {
			struct state *st;
			if (mdlist->devname == NULL)
				continue;
			if (strcasecmp(mdlist->devname, "<ignore>") == 0)
				continue;
			st = calloc(1, sizeof *st);
			if (st == NULL)
				continue;
			if (mdlist->devname[0] == '/')
				st->devname = strdup(mdlist->devname);
			else {
				st->devname = malloc(8+strlen(mdlist->devname)+1);
				strcpy(strcpy(st->devname, "/dev/md/"),
				       mdlist->devname);
			}
			st->next = statelist;
			st->devnum = INT_MAX;
			st->percent = -2;
			st->expected_spares = mdlist->spare_disks;
			if (mdlist->spare_group)
				st->spare_group = strdup(mdlist->spare_group);
			statelist = st;
		}
	} else {
		struct mddev_dev *dv;
		for (dv=devlist ; dv; dv=dv->next) {
			struct mddev_ident *mdlist = conf_get_ident(dv->devname);
			struct state *st = calloc(1, sizeof *st);
			if (st == NULL)
				continue;
			st->devname = strdup(dv->devname);
			st->next = statelist;
			st->devnum = INT_MAX;
			st->percent = -2;
			st->expected_spares = -1;
			if (mdlist) {
				st->expected_spares = mdlist->spare_disks;
				if (mdlist->spare_group)
					st->spare_group = strdup(mdlist->spare_group);
			}
			statelist = st;
		}
	}


	while (! finished) {
		int new_found = 0;
		struct state *st;
		int anydegraded = 0;

		if (mdstat)
			free_mdstat(mdstat);
		mdstat = mdstat_read(oneshot?0:1, 0);

		for (st=statelist; st; st=st->next)
			if (check_array(st, mdstat, test, &info, increments))
				anydegraded = 1;
		
		/* now check if there are any new devices found in mdstat */
		if (scan)
			new_found = add_new_arrays(mdstat, statelist, test,
						   &info);

		/* If an array has active < raid && spare == 0 && spare_group != NULL
		 * Look for another array with spare > 0 and active == raid and same spare_group
		 *  if found, choose a device and hotremove/hotadd
		 */
		if (share && anydegraded)
			try_spare_migration(statelist, &info);
		if (!new_found) {
			if (oneshot)
				break;
			else
				mdstat_wait(period);
		}
		test = 0;
	}
	if (pidfile)
		unlink(pidfile);
	return 0;
}

static int make_daemon(char *pidfile)
{
	int pid = fork();
	if (pid > 0) {
		if (!pidfile)
			printf("%d\n", pid);
		else {
			FILE *pid_file;
			pid_file=fopen(pidfile, "w");
			if (!pid_file)
				perror("cannot create pid file");
			else {
				fprintf(pid_file,"%d\n", pid);
				fclose(pid_file);
			}
		}
		return 0;
	}
	if (pid < 0) {
		perror("daemonise");
		return 1;
	}
	close(0);
	open("/dev/null", O_RDWR);
	dup2(0,1);
	dup2(0,2);
	setsid();
	return 0;
}

static int check_one_sharer(int scan)
{
	int pid, rv;
	FILE *fp;
	char dir[20];
	struct stat buf;
	fp = fopen("/var/run/mdadm/autorebuild.pid", "r");
	if (fp) {
		fscanf(fp, "%d", &pid);
		sprintf(dir, "/proc/%d", pid);
		rv = stat(dir, &buf);
		if (rv != -1) {
			if (scan) {
				fprintf(stderr, Name ": Only one "
					"autorebuild process allowed"
					" in scan mode, aborting\n");
				fclose(fp);
				return 1;
			} else {
				fprintf(stderr, Name ": Warning: One"
					" autorebuild process already"
					" running.");
			}
		}
		fclose(fp);
	}
	if (scan) {
		fp = fopen("/var/run/mdadm/autorebuild.pid", "w");
		if (!fp)
			fprintf(stderr, Name ": Cannot create"
				" autorebuild.pid "
				"file\n");
		else {
			pid = getpid();
			fprintf(fp, "%d\n", pid);
			fclose(fp);
		}
	}
	return 0;
}

static void alert(char *event, char *dev, char *disc, struct alert_info *info)
{
	int priority;

	if (!info->alert_cmd && !info->mailaddr) {
		time_t now = time(0);

		printf("%1.15s: %s on %s %s\n", ctime(&now)+4, event, dev, disc?disc:"unknown device");
	}
	if (info->alert_cmd) {
		int pid = fork();
		switch(pid) {
		default:
			waitpid(pid, NULL, 0);
			break;
		case -1:
			break;
		case 0:
			execl(info->alert_cmd, info->alert_cmd,
			      event, dev, disc, NULL);
			exit(2);
		}
	}
	if (info->mailaddr &&
	    (strncmp(event, "Fail", 4)==0 ||
	     strncmp(event, "Test", 4)==0 ||
	     strncmp(event, "Spares", 6)==0 ||
	     strncmp(event, "Degrade", 7)==0)) {
		FILE *mp = popen(Sendmail, "w");
		if (mp) {
			FILE *mdstat;
			char hname[256];
			gethostname(hname, sizeof(hname));
			signal(SIGPIPE, SIG_IGN);
			if (info->mailfrom)
				fprintf(mp, "From: %s\n", info->mailfrom);
			else
				fprintf(mp, "From: " Name " monitoring <root>\n");
			fprintf(mp, "To: %s\n", info->mailaddr);
			fprintf(mp, "Subject: %s event on %s:%s\n\n",
				event, dev, hname);

			fprintf(mp,
				"This is an automatically generated"
				" mail message from " Name "\n");
			fprintf(mp, "running on %s\n\n", hname);

			fprintf(mp,
				"A %s event had been detected on"
				" md device %s.\n\n", event, dev);

			if (disc && disc[0] != ' ')
				fprintf(mp,
					"It could be related to"
					" component device %s.\n\n", disc);
			if (disc && disc[0] == ' ')
				fprintf(mp, "Extra information:%s.\n\n", disc);

			fprintf(mp, "Faithfully yours, etc.\n");

			mdstat = fopen("/proc/mdstat", "r");
			if (mdstat) {
				char buf[8192];
				int n;
				fprintf(mp,
					"\nP.S. The /proc/mdstat file"
					" currently contains the following:\n\n");
				while ( (n=fread(buf, 1, sizeof(buf), mdstat)) > 0)
					n=fwrite(buf, 1, n, mp);
				fclose(mdstat);
			}
			pclose(mp);
		}
	}

	/* log the event to syslog maybe */
	if (info->dosyslog) {
		/* Log at a different severity depending on the event.
		 *
		 * These are the critical events:  */
		if (strncmp(event, "Fail", 4)==0 ||
		    strncmp(event, "Degrade", 7)==0 ||
		    strncmp(event, "DeviceDisappeared", 17)==0)
			priority = LOG_CRIT;
		/* Good to know about, but are not failures: */
		else if (strncmp(event, "Rebuild", 7)==0 ||
			 strncmp(event, "MoveSpare", 9)==0 ||
			 strncmp(event, "Spares", 6) != 0)
			priority = LOG_WARNING;
		/* Everything else: */
		else
			priority = LOG_INFO;

		if (disc)
			syslog(priority,
			       "%s event detected on md device %s,"
			       " component device %s", event, dev, disc);
		else
			syslog(priority,
			       "%s event detected on md device %s",
			       event, dev);
	}
}

static int check_array(struct state *st, struct mdstat_ent *mdstat,
		       int test, struct alert_info *ainfo,
		       int increments)
{
	struct { int state, major, minor; } info[MaxDisks];
	mdu_array_info_t array;
	struct mdstat_ent *mse = NULL, *mse2;
	char *dev = st->devname;
	int fd;
	int i;

	if (test)
		alert("TestMessage", dev, NULL, ainfo);
	fd = open(dev, O_RDONLY);
	if (fd < 0) {
		if (!st->err)
			alert("DeviceDisappeared", dev, NULL, ainfo);
		st->err=1;
		return 0;
	}
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	if (ioctl(fd, GET_ARRAY_INFO, &array)<0) {
		if (!st->err)
			alert("DeviceDisappeared", dev, NULL, ainfo);
		st->err=1;
		close(fd);
		return 0;
	}
	/* It's much easier to list what array levels can't
	 * have a device disappear than all of them that can
	 */
	if (array.level == 0 || array.level == -1) {
		if (!st->err)
			alert("DeviceDisappeared", dev, "Wrong-Level", ainfo);
		st->err = 1;
		close(fd);
		return 0;
	}
	if (st->devnum == INT_MAX) {
		struct stat stb;
		if (fstat(fd, &stb) == 0 &&
		    (S_IFMT&stb.st_mode)==S_IFBLK) {
			if (major(stb.st_rdev) == MD_MAJOR)
				st->devnum = minor(stb.st_rdev);
			else
				st->devnum = -1- (minor(stb.st_rdev)>>6);
		}
	}

	for (mse2 = mdstat ; mse2 ; mse2=mse2->next)
		if (mse2->devnum == st->devnum) {
			mse2->devnum = INT_MAX; /* flag it as "used" */
			mse = mse2;
		}

	if (!mse) {
		/* duplicated array in statelist
		 * or re-created after reading mdstat*/
		st->err = 1;
		close(fd);
		return 0;
	}
	/* this array is in /proc/mdstat */
	if (array.utime == 0)
		/* external arrays don't update utime, so
		 * just make sure it is always different. */
		array.utime = st->utime + 1;;

	if (st->utime == array.utime &&
	    st->failed == array.failed_disks &&
	    st->working == array.working_disks &&
	    st->spare == array.spare_disks &&
	    (mse == NULL  || (
		    mse->percent == st->percent
		    ))) {
		close(fd);
		st->err = 0;
		return 0;
	}
	if (st->utime == 0 && /* new array */
	    mse->pattern && strchr(mse->pattern, '_') /* degraded */
		)
		alert("DegradedArray", dev, NULL, ainfo);

	if (st->utime == 0 && /* new array */
	    st->expected_spares > 0 &&
	    array.spare_disks < st->expected_spares)
		alert("SparesMissing", dev, NULL, ainfo);
	if (st->percent == -1 &&
	    mse->percent >= 0)
		alert("RebuildStarted", dev, NULL, ainfo);
	if (st->percent >= 0 &&
	    mse->percent >= 0 &&
	    (mse->percent / increments) > (st->percent / increments)) {
		char percentalert[15]; // "RebuildNN" (10 chars) or "RebuildStarted" (15 chars)

		if((mse->percent / increments) == 0)
			snprintf(percentalert, sizeof(percentalert), "RebuildStarted");
		else
			snprintf(percentalert, sizeof(percentalert), "Rebuild%02d", mse->percent);

		alert(percentalert, dev, NULL, ainfo);
	}

	if (mse->percent == -1 &&
	    st->percent >= 0) {
		/* Rebuild/sync/whatever just finished.
		 * If there is a number in /mismatch_cnt,
		 * we should report that.
		 */
		struct mdinfo *sra =
			sysfs_read(-1, st->devnum, GET_MISMATCH);
		if (sra && sra->mismatch_cnt > 0) {
			char cnt[40];
			sprintf(cnt, " mismatches found: %d", sra->mismatch_cnt);
			alert("RebuildFinished", dev, cnt, ainfo);
		} else
			alert("RebuildFinished", dev, NULL, ainfo);
		if (sra)
			free(sra);
	}
	st->percent = mse->percent;

	for (i=0; i<MaxDisks && i <= array.raid_disks + array.nr_disks;
	     i++) {
		mdu_disk_info_t disc;
		disc.number = i;
		if (ioctl(fd, GET_DISK_INFO, &disc) >= 0) {
			info[i].state = disc.state;
			info[i].major = disc.major;
			info[i].minor = disc.minor;
		} else
			info[i].major = info[i].minor = 0;
	}

	if (strncmp(mse->metadata_version, "external:", 9) == 0 &&
	    is_subarray(mse->metadata_version+9))
		st->parent_dev =
			devname2devnum(mse->metadata_version+10);
	else
		st->parent_dev = NoMdDev;
	if (st->metadata == NULL &&
	    st->parent_dev == NoMdDev)
		st->metadata = super_by_fd(fd, NULL);

	close(fd);

	for (i=0; i<MaxDisks; i++) {
		mdu_disk_info_t disc = {0,0,0,0,0};
		int newstate=0;
		int change;
		char *dv = NULL;
		disc.number = i;
		if (i > array.raid_disks + array.nr_disks) {
			newstate = 0;
			disc.major = disc.minor = 0;
		} else if (info[i].major || info[i].minor) {
			newstate = info[i].state;
			dv = map_dev(info[i].major, info[i].minor, 1);
			disc.state = newstate;
			disc.major = info[i].major;
			disc.minor = info[i].minor;
		} else if (mse &&  mse->pattern && i < (int)strlen(mse->pattern)) {
			switch(mse->pattern[i]) {
			case 'U': newstate = 6 /* ACTIVE/SYNC */; break;
			case '_': newstate = 0; break;
			}
			disc.major = disc.minor = 0;
		}
		if (dv == NULL && st->devid[i])
			dv = map_dev(major(st->devid[i]),
				     minor(st->devid[i]), 1);
		change = newstate ^ st->devstate[i];
		if (st->utime && change && !st->err) {
			if (i < array.raid_disks &&
			    (((newstate&change)&(1<<MD_DISK_FAULTY)) ||
			     ((st->devstate[i]&change)&(1<<MD_DISK_ACTIVE)) ||
			     ((st->devstate[i]&change)&(1<<MD_DISK_SYNC)))
				)
				alert("Fail", dev, dv, ainfo);
			else if (i >= array.raid_disks &&
				 (disc.major || disc.minor) &&
				 st->devid[i] == makedev(disc.major, disc.minor) &&
				 ((newstate&change)&(1<<MD_DISK_FAULTY))
				)
				alert("FailSpare", dev, dv, ainfo);
			else if (i < array.raid_disks &&
				 ! (newstate & (1<<MD_DISK_REMOVED)) &&
				 (((st->devstate[i]&change)&(1<<MD_DISK_FAULTY)) ||
				  ((newstate&change)&(1<<MD_DISK_ACTIVE)) ||
				  ((newstate&change)&(1<<MD_DISK_SYNC)))
				)
				alert("SpareActive", dev, dv, ainfo);
		}
		st->devstate[i] = newstate;
		st->devid[i] = makedev(disc.major, disc.minor);
	}
	st->active = array.active_disks;
	st->working = array.working_disks;
	st->spare = array.spare_disks;
	st->failed = array.failed_disks;
	st->utime = array.utime;
	st->raid = array.raid_disks;
	st->err = 0;
	if ((st->active < st->raid) && st->spare == 0)
		return 1;
	return 0;
}

static int add_new_arrays(struct mdstat_ent *mdstat, struct state *statelist,
			  int test, struct alert_info *info)
{
	struct mdstat_ent *mse;
	int new_found = 0;

	for (mse=mdstat; mse; mse=mse->next)
		if (mse->devnum != INT_MAX &&
		    (!mse->level  || /* retrieve containers */
		     (strcmp(mse->level, "raid0") != 0 &&
		      strcmp(mse->level, "linear") != 0))
			) {
			struct state *st = calloc(1, sizeof *st);
			mdu_array_info_t array;
			int fd;
			if (st == NULL)
				continue;
			st->devname = strdup(get_md_name(mse->devnum));
			if ((fd = open(st->devname, O_RDONLY)) < 0 ||
			    ioctl(fd, GET_ARRAY_INFO, &array)< 0) {
				/* no such array */
				if (fd >=0) close(fd);
				put_md_name(st->devname);
				free(st->devname);
				if (st->metadata) {
					st->metadata->ss->free_super(st->metadata);
					free(st->metadata);
				}
				free(st);
				continue;
			}
			close(fd);
			st->next = statelist;
			st->err = 1;
			st->devnum = mse->devnum;
			st->percent = -2;
			st->expected_spares = -1;
			if (strncmp(mse->metadata_version, "external:", 9) == 0 &&
			    is_subarray(mse->metadata_version+9))
				st->parent_dev =
					devname2devnum(mse->metadata_version+10);
			else
				st->parent_dev = NoMdDev;
			statelist = st;
			if (test)
				alert("TestMessage", st->devname, NULL, info);
			alert("NewArray", st->devname, NULL, info);
			new_found = 1;
		}
	return new_found;
}

unsigned long long min_spare_size_required(struct state *st)
{
	int fd;
	unsigned long long rv = 0;

	if (!st->metadata ||
	    !st->metadata->ss->min_acceptable_spare_size)
		return rv;

	fd = open(st->devname, O_RDONLY);
	if (fd < 0)
		return 0;
	st->metadata->ss->load_super(st->metadata, fd, st->devname);
	close(fd);
	rv = st->metadata->ss->min_acceptable_spare_size(st->metadata);
	st->metadata->ss->free_super(st->metadata);

	return rv;
}

static int move_spare(struct state *from, struct state *to,
		      int devid,
		      struct alert_info *info)
{
	struct mddev_dev devlist;
	char devname[20];

	/* try to remove and add */
	int fd1 = open(to->devname, O_RDONLY);
	int fd2 = open(from->devname, O_RDONLY);

	if (fd1 < 0 || fd2 < 0) {
		if (fd1>=0) close(fd1);
		if (fd2>=0) close(fd2);
		return 0;
	}

	devlist.next = NULL;
	devlist.used = 0;
	devlist.re_add = 0;
	devlist.writemostly = 0;
	devlist.devname = devname;
	sprintf(devname, "%d:%d", major(devid), minor(devid));

	devlist.disposition = 'r';
	if (Manage_subdevs(from->devname, fd2, &devlist, -1, 0) == 0) {
		devlist.disposition = 'a';
		if (Manage_subdevs(to->devname, fd1, &devlist, -1, 0) == 0) {
			alert("MoveSpare", to->devname, from->devname, info);
			close(fd1);
			close(fd2);
			return 1;
		}
		else Manage_subdevs(from->devname, fd2, &devlist, -1, 0);
	}
	close(fd1);
	close(fd2);
	return 0;
}

static int check_donor(struct state *from, struct state *to,
		       struct domainlist *domlist)
{
	struct state *sub;

	if (from == to)
		return 0;
	if (from->parent)
		/* Cannot move from a member */
		return 0;
	for (sub = from->subarray; sub; sub = sub->subarray)
		/* If source array has degraded subarrays, don't
		 * remove anything
		 */
		if (sub->active < sub->raid)
			return 0;
	if (from->metadata->ss->external == 0)
		if (from->active < from->raid)
			return 0;
	if (from->spare <= 0)
		return 0;
	if (domlist == NULL)
		return 0;
	return 1;
}

static int choose_spare(struct state *from, struct state *to,
			struct domainlist *domlist)
{
	int d;
	int dev = 0;
	unsigned long long min_size
		= min_spare_size_required(to);

	for (d = from->raid; !dev && d < MaxDisks; d++) {
		if (from->devid[d] > 0 &&
		    from->devstate[d] == 0) {
			struct dev_policy *pol;
			unsigned long long dev_size;

			if (min_size &&
			    dev_size_from_id(from->devid[d], &dev_size) &&
			    dev_size < min_size)
				continue;

			pol = devnum_policy(from->devid[d]);
			if (from->spare_group)
				pol_add(&pol, pol_domain,
					from->spare_group, NULL);
			if (domain_test(domlist, pol, to->metadata->ss->name))
			    dev = from->devid[d];
			dev_policy_free(pol);
		}
	}
	return dev;
}

static void try_spare_migration(struct state *statelist, struct alert_info *info)
{
	struct state *from;
	struct state *st;

	link_containers_with_subarrays(statelist);
	for (st = statelist; st; st = st->next)
		if (st->active < st->raid &&
		    st->spare == 0) {
			struct domainlist *domlist = NULL;
			int d;
			struct state *to = st;

			if (to->parent)
				/* member of a container */
				to = to->parent;

			for (d = 0; d < MaxDisks; d++)
				if (to->devid[d])
					domainlist_add_dev(&domlist,
							   to->devid[d],
							   to->metadata->ss->name);
			if (to->spare_group)
				domain_add(&domlist, to->spare_group);

			for (from=statelist ; from ; from=from->next) {
				int devid;
				if (!check_donor(from, to, domlist))
					continue;
				devid = choose_spare(from, to, domlist);
				if (devid > 0
				    && move_spare(from, to, devid, info))
						break;
			}
			domain_free(domlist);
		}
}

/* search the statelist to connect external
 * metadata subarrays with their containers
 * We always completely rebuild the tree from scratch as
 * that is safest considering the possibility of entries
 * disappearing or changing.
 */
static void link_containers_with_subarrays(struct state *list)
{
	struct state *st;
	struct state *cont;
	for (st = list; st; st = st->next) {
		st->parent = NULL;
		st->subarray = NULL;
	}
	for (st = list; st; st = st->next)
		if (st->parent_dev != NoMdDev)
			for (cont = list; cont; cont = cont->next)
				if (!cont->err &&
				    cont->parent_dev == NoMdDev &&
				    cont->devnum == st->parent_dev) {
					st->parent = cont;
					st->subarray = cont->subarray;
					cont->subarray = st;
					break;
				}
}

/* Not really Monitor but ... */
int Wait(char *dev)
{
	struct stat stb;
	int devnum;
	int rv = 1;

	if (stat(dev, &stb) != 0) {
		fprintf(stderr, Name ": Cannot find %s: %s\n", dev,
			strerror(errno));
		return 2;
	}
	devnum = stat2devnum(&stb);

	while(1) {
		struct mdstat_ent *ms = mdstat_read(1, 0);
		struct mdstat_ent *e;

		for (e=ms ; e; e=e->next)
			if (e->devnum == devnum)
				break;

		if (!e || e->percent < 0) {
			if (e && e->metadata_version &&
			    strncmp(e->metadata_version, "external:", 9) == 0) {
				if (is_subarray(&e->metadata_version[9]))
					ping_monitor(&e->metadata_version[9]);
				else
					ping_monitor(devnum2devname(devnum));
			}
			free_mdstat(ms);
			return rv;
		}
		free_mdstat(ms);
		rv = 0;
		mdstat_wait(5);
	}
}
