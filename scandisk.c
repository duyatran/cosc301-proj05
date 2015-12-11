#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <ctype.h>

#include "bootsect.h"
#include "bpb.h"
#include "direntry.h"
#include "fat.h"
#include "dos.h"

void write_dirent(struct direntry *dirent, char *filename, 
		  uint16_t start_cluster, int *used_clusters, 
		  uint8_t *image_buf, struct bpb33* bpb)
{
    char *p, *p2;
    char *uppername;
    int len, i;

    /* clean out anything old that used to be here */
    memset(dirent, 0, sizeof(struct direntry));

    /* extract just the filename part */
    uppername = strdup(filename);
    p2 = uppername;
    for (i = 0; i < strlen(filename); i++) 
    {
	if (p2[i] == '/' || p2[i] == '\\') 
	{
	    uppername = p2+i+1;
	}
    }

    /* convert filename to upper case */
    for (i = 0; i < strlen(uppername); i++) 
    {
	uppername[i] = toupper(uppername[i]);
    }

    /* set the file name and extension */
    memset(dirent->deName, ' ', 8);
    p = strchr(uppername, '.');
    memcpy(dirent->deExtension, "___", 3);
    if (p == NULL) 
    {
	fprintf(stderr, "No filename extension given - defaulting to .___\n");
    }
    else 
    {
	*p = '\0';
	p++;
	len = strlen(p);
	if (len > 3) len = 3;
	memcpy(dirent->deExtension, p, len);
    }

    if (strlen(uppername)>8) 
    {
	uppername[8]='\0';
    }
    memcpy(dirent->deName, uppername, strlen(uppername));
    free(p2);

	uint16_t cluster = start_cluster;
	int count = 0;
	printf("Orphan, starting cluster %d\n", start_cluster);

    while (is_valid_cluster(cluster, bpb) && !is_end_of_file(cluster))
    {
		//printf("Another block in this orphan file: %d\n", cluster);
		count++;
		used_clusters[cluster] = 1;
		cluster = get_fat_entry(cluster, image_buf, bpb);
    }
	int size = count * (bpb->bpbBytesPerSec) * (bpb->bpbSecPerClust);
    /* set the attributes and file size */
    dirent->deAttributes = ATTR_NORMAL;
    putushort(dirent->deStartCluster, start_cluster);
    putulong(dirent->deFileSize, size);
	

    /* could also set time and date here if we really
       cared... */
}

void create_dirent(struct direntry *dirent, char *filename, 
		   uint16_t start_cluster, int *used_clusters,
		   uint8_t *image_buf, struct bpb33* bpb)
{
    while (1) 
    {
	if (dirent->deName[0] == SLOT_EMPTY) 
	{
	    /* we found an empty slot at the end of the directory */
	    write_dirent(dirent, filename, start_cluster, used_clusters, image_buf, bpb);
	    dirent++;

	    /* make sure the next dirent is set to be empty, just in
	       case it wasn't before */
	    memset((uint8_t*)dirent, 0, sizeof(struct direntry));
	    dirent->deName[0] = SLOT_EMPTY;
	    return;
	}

	if (dirent->deName[0] == SLOT_DELETED) 
	{
	    /* we found a deleted entry - we can just overwrite it */
	    write_dirent(dirent, filename, start_cluster, used_clusters, image_buf, bpb);
	    return;
	}
	dirent++;
    }
}

uint16_t clusters_len(uint16_t cluster, 
			uint8_t *image_buf, struct bpb33 *bpb, int *used_sectors) 
{
	int len = 0;
	uint16_t next_cluster = 0;

    while (is_valid_cluster(cluster, bpb) && !is_end_of_file(cluster))
    {
		len++;
		used_sectors[cluster] = 1;
		next_cluster = get_fat_entry(cluster, image_buf, bpb);

		if (cluster == (FAT12_MASK&CLUST_BAD) 
			|| cluster == next_cluster
			|| !is_valid_cluster(next_cluster, bpb)) {
			set_fat_entry(cluster, FAT12_MASK&CLUST_EOFS, image_buf, bpb);
			return len;
		}
		cluster = next_cluster;
    }
    return len;
}

void fix_sz(struct direntry *dirent,
		uint8_t *image_buf, struct bpb33* bpb) 
{	
	uint16_t current = getushort(dirent->deStartCluster);
	uint32_t dirent_size = getulong(dirent->deFileSize);
	uint16_t cluster_size = bpb->bpbBytesPerSec * bpb->bpbSecPerClust;

	uint16_t next;
	
	int metadata_len = (dirent_size + cluster_size - 1)/cluster_size;
	uint16_t last_metadata_cluster = metadata_len + current - 1;

	int count = 0;
	while (is_valid_cluster(current, bpb)) {
		next = get_fat_entry(current, image_buf, bpb);
		count++;
		if (current == last_metadata_cluster) {
			set_fat_entry(current, FAT12_MASK&CLUST_EOFS, image_buf, bpb);
		}
		else if (current > last_metadata_cluster) {
			set_fat_entry(current, FAT12_MASK&CLUST_FREE, image_buf, bpb);
		}
        if (is_end_of_file(next)) {
            break;
        }
        current = next;
	}
	uint16_t last_fat_cluster = count + getushort(dirent->deStartCluster) - 1;

	if (last_metadata_cluster > last_fat_cluster) {
		uint32_t change = (cluster_size * (last_fat_cluster-last_metadata_cluster));
		dirent_size += change;
		putulong(dirent->deFileSize, dirent_size);
	}
}

int check_inconsistency(struct direntry *dirent, uint8_t *image_buf, 
			struct bpb33* bpb, int *used_sectors) {
	
	uint16_t start_cluster = getushort(dirent->deStartCluster);
	uint32_t dirent_sz = getulong(dirent->deFileSize);
	uint16_t cluster_size = bpb->bpbBytesPerSec * bpb->bpbSecPerClust;

	uint16_t metadata_len = (dirent_sz + cluster_size - 1)/cluster_size;
	uint16_t cluster_len = clusters_len(start_cluster, image_buf, bpb, used_sectors);

	printf("start cluster is %d blocks\n", start_cluster);
	printf("metadata_size is %d blocks\n", metadata_len);
	printf("cluster_chain is %d blocks\n", cluster_len);

	if (metadata_len != cluster_len) {
		return 1;
	}

	return 0;
}

void print_indent(int indent)
{
    int i;
    for (i = 0; i < indent*4; i++)
	printf(" ");
}

int check_fix_invalid_file(struct direntry *dirent, struct bpb33* bpb) {
    uint16_t start_cluster = getushort(dirent->deStartCluster);
	if (!is_valid_cluster(start_cluster, bpb)) {
		memset(dirent, 0, sizeof(struct direntry));
		dirent->deName[0] = SLOT_EMPTY;
		return 0;
	}
	return 1;
}

uint16_t read_dirent(struct direntry *dirent, int indent, 
			uint8_t *image_buf, struct bpb33* bpb, int *used_clusters)
{
	uint16_t followclust = 0;

    int i;
    char name[9];
    char extension[4];
    uint16_t file_cluster;
    uint32_t size;
    name[8] = ' ';
    extension[3] = ' ';
    memcpy(name, &(dirent->deName[0]), 8);
    memcpy(extension, dirent->deExtension, 3);
    
    /* names are space padded - remove the spaces */
    for (i = 8; i > 0; i--) 
    {
	if (name[i] == ' ') 
	    name[i] = '\0';
	else 
	    break;
    }

    /* remove the spaces from extensions */
    for (i = 3; i > 0; i--) 
    {
	if (extension[i] == ' ') 
	    extension[i] = '\0';
	else 
	    break;
    }
	
	if (name[0] == SLOT_EMPTY)
    {
	return followclust;
    }

    /* skip over deleted entries */
    if (((uint8_t)name[0]) == SLOT_DELETED)
    {
	return followclust;
    }

    if (((uint8_t)name[0]) == 0x2E)
    {
	// dot entry ("." or "..")
	// skip it
        return followclust;
    }
	
    if ((dirent->deAttributes & ATTR_WIN95LFN) == ATTR_WIN95LFN)
    {
	// ignore any long file name extension entries
	//
	// printf("Win95 long-filename entry seq 0x%0x\n", dirent->deName[0]);
    }
    else if ((dirent->deAttributes & ATTR_VOLUME) != 0) 
    {
		printf("Volume: %s\n", name);
    } 
    else if ((dirent->deAttributes & ATTR_DIRECTORY) != 0) 
    {
        // don't deal with hidden directories; MacOS makes these
        // for trash directories and such; just ignore them.
		if ((dirent->deAttributes & ATTR_HIDDEN) != ATTR_HIDDEN) {
		    file_cluster = getushort(dirent->deStartCluster);
            followclust = file_cluster;
			used_clusters[followclust] = 1;
		}
    }
    else {
		/*
		 * a "regular" file entry
		 * print attributes, size, starting cluster, etc.
		 */
		int ro = (dirent->deAttributes & ATTR_READONLY) == ATTR_READONLY;
		int hidden = (dirent->deAttributes & ATTR_HIDDEN) == ATTR_HIDDEN;
		int sys = (dirent->deAttributes & ATTR_SYSTEM) == ATTR_SYSTEM;
		int arch = (dirent->deAttributes & ATTR_ARCHIVE) == ATTR_ARCHIVE;

		size = getulong(dirent->deFileSize);
		print_indent(indent);
		printf("%s.%s (%u bytes) (starting cluster %d) %c%c%c%c\n", 
			   name, extension, size, getushort(dirent->deStartCluster),
			   ro?'r':' ', 
				   hidden?'h':' ', 
				   sys?'s':' ', 
				   arch?'a':' ');
				   
		int valid = check_fix_invalid_file(dirent, bpb);
		if (valid && check_inconsistency(dirent, image_buf, bpb, used_clusters)) {
			printf("Some inconsistency with the file above\n");
			fix_sz(dirent, image_buf, bpb);
		}
	}	
    return followclust;
}

void follow_dir(uint16_t cluster, int indent,
		uint8_t *image_buf, struct bpb33* bpb, int *used_clusters) {
    
    while (is_valid_cluster(cluster, bpb)) {
        struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

        int numDirEntries = (bpb->bpbBytesPerSec * bpb->bpbSecPerClust) / sizeof(struct direntry);
        int i = 0;
		for ( ; i < numDirEntries; i++) {
			uint16_t followclust = read_dirent(dirent, 0, image_buf, bpb, used_clusters);
			if (followclust)
				follow_dir(followclust, indent+1, image_buf, bpb, used_clusters);
			dirent++;
		}

	cluster = get_fat_entry(cluster, image_buf, bpb);
    }
}


void traverse_root(uint8_t *image_buf, struct bpb33* bpb,
					int *used_clusters)
{
    uint16_t cluster = 0;
    struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);
    int i = 0;

    for ( ; i < bpb->bpbRootDirEnts; i++)
    {
        uint16_t followclust = read_dirent(dirent, 0, image_buf, bpb, used_clusters);
        if (is_valid_cluster(followclust, bpb)) {
            follow_dir(followclust, 1, image_buf, bpb, used_clusters);
		}
        dirent++;
    }
}

void find_orphans(uint8_t *image_buf, 
		struct bpb33* bpb, int *used_clusters) 
{
	struct direntry *root_dir = (struct direntry*)cluster_to_addr(MSDOSFSROOT, image_buf, bpb);
	char orphan_name[64];
	int count = 0;
	int i = 0;
	int start_clusters[bpb->bpbSectors];
	
	for (; i < bpb->bpbSectors; i++) {
		start_clusters[i] = 1;
	}
	
	// A start-of-file cluster will is not linked to 
	// by any other cluster, and its value in the start_clusters 
	// array will remain 1.
	for (i = CLUST_FIRST; i < bpb->bpbSectors; i++) {
		uint16_t ref = get_fat_entry(i, image_buf, bpb);
		start_clusters[ref] = 0; 
	}
	
    for (i = CLUST_FIRST; i < bpb->bpbSectors; i++) {
		if ((used_clusters[i] == 0)
			&& (get_fat_entry(i, image_buf, bpb) != (FAT12_MASK&CLUST_FREE))
			&& (start_clusters[i] == 1))
		{	
			count++;
			snprintf(orphan_name, 64, "found%d.dat", count);
			create_dirent(root_dir, orphan_name, i, 
					used_clusters, image_buf, bpb);
		}
	}
}

void usage(char *progname) {
    fprintf(stderr, "usage: %s <imagename>\n", progname);
    exit(1);
}


int main(int argc, char** argv) {
    uint8_t *image_buf;
    int fd;
    struct bpb33* bpb;
    if (argc < 2) {
	usage(argv[0]);
    }

    image_buf = mmap_file(argv[1], &fd);
    bpb = check_bootsector(image_buf);

    // your code should start here...
    int used_clusters[bpb->bpbSectors];
    for (int i = 0; i < bpb->bpbSectors; i++) {
		used_clusters[i] = 0;
	}
	traverse_root(image_buf, bpb, used_clusters);
	find_orphans(image_buf, bpb, used_clusters);


    unmmap_file(image_buf, &fd);
    return 0;
}
