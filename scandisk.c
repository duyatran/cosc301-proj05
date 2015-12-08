#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>

#include "bootsect.h"
#include "bpb.h"
#include "direntry.h"
#include "fat.h"
#include "dos.h"
#define NO_PRINT 0
#define PRINT 1

int dirent_clusters(struct direntry *dirent, struct bpb33 *bpb) 
{
	uint32_t dirent_sz = getulong(dirent->deFileSize);
	uint16_t cluster_size = bpb->bpbBytesPerSec * bpb->bpbSecPerClust;

	int num_blocks = dirent_sz/cluster_size;
	if (dirent_sz % cluster_size != 0) {
		num_blocks++;
	}
	return num_blocks;
}

int chain_length(struct direntry *dirent, uint8_t *image_buf, struct bpb33 *bpb) 
{
    uint16_t cluster = getushort(dirent->deStartCluster);
	int count = 0;

    while (is_valid_cluster(cluster, bpb))
    {
		count++;
		// break if current cluster is eof
        if (is_end_of_file(cluster)) {
			break;
		}
		cluster = get_fat_entry(cluster, image_buf, bpb);
    }
    return count;
}

void fix_file_size(struct direntry *dirent, 
		uint8_t *image_buf, struct bpb33* bpb, int meta_sz, int chain_sz) 
{	
	int count = 0;
	uint16_t cluster = getushort(dirent->deStartCluster);
	uint16_t end_cluster;

    while (is_valid_cluster(cluster, bpb)) {
		count++;
		if (count == meta_sz) {
			end_cluster = cluster;
		}
		if (count > meta_sz) {
			set_fat_entry(cluster, CLUST_FREE, image_buf, bpb);
		}	
		cluster = get_fat_entry(cluster, image_buf, bpb);
    }
    set_fat_entry(end_cluster, FAT12_MASK&CLUST_EOFS, image_buf, bpb);
}

void print_indent(int indent)
{
    int i;
    for (i = 0; i < indent*4; i++)
	printf(" ");
}

uint16_t read_dirent(struct direntry *dirent, int indent, int print_flag)
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
    else if (print_flag && (dirent->deAttributes & ATTR_VOLUME) != 0) 
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
			if (print_flag) {
				print_indent(indent);
				printf("%s/ (directory)\n", name);
			}
		}
    }
    else if (print_flag)
    {
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
    }
    return followclust;
}

void follow_dir(uint16_t cluster, int indent,
		uint8_t *image_buf, struct bpb33* bpb, int *sz_err)
{
    while (is_valid_cluster(cluster, bpb))
    {
        struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

        int numDirEntries = (bpb->bpbBytesPerSec * bpb->bpbSecPerClust) / sizeof(struct direntry);
        int i = 0;
	for ( ; i < numDirEntries; i++)
	{
			uint16_t followclust = read_dirent(dirent, 0, NO_PRINT);
            int metadata_size = dirent_clusters(dirent, bpb);
            int cluster_chain = chain_length(dirent, image_buf, bpb);
            
            if ((metadata_size != cluster_chain) && 
				(dirent->deAttributes & ATTR_DIRECTORY) == 0) {
				*sz_err = 1;
				read_dirent(dirent, indent, PRINT);
				//fix_file_size(dirent, image_buf, bpb, metadata_size, cluster_chain);
				printf("metadata_size is %d blocks\n", dirent_clusters(dirent, bpb));
				printf("cluster_chain is %d blocks\n", chain_length(dirent, image_buf, bpb));
			}
            
            if (followclust)
                follow_dir(followclust, indent+1, image_buf, bpb, sz_err);
            dirent++;
	}

	cluster = get_fat_entry(cluster, image_buf, bpb);
    }
}


void check_size_consistency(uint8_t *image_buf, struct bpb33* bpb)
{
    uint16_t cluster = 0;
	int rv = 0;
    struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

    int i = 0;
    printf("Files whose metadata size is different from chain size:\n");

    for ( ; i < bpb->bpbRootDirEnts; i++)
    {
        uint16_t followclust = read_dirent(dirent, 0, NO_PRINT);
        if (is_valid_cluster(followclust, bpb)) {
            follow_dir(followclust, 1, image_buf, bpb, &rv);
		}
        dirent++;
    }
    if (!rv) printf("No size inconsistency.\n");
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
	check_size_consistency(image_buf, bpb);





    unmmap_file(image_buf, &fd);
    return 0;
}
