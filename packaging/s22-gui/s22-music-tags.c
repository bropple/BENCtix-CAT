// SPDX-License-Identifier: MIT
/*
 * s22-music-tags: the tags of every song in ~/Music, for Music's Artists
 * and Albums (D083).
 *
 *   s22-music-tags MUSIC_DIR CACHE
 *
 * Reads the artist, album artist, album, track, disc, year and title of
 * each song with ffmpeg's libavformat (headers only: ~35 ms a song here, a
 * first look at 700 songs takes half a minute), and keeps them in CACHE,
 * one song a line:
 *
 *   PATH TAB MTIME TAB SIZE TAB ARTIST TAB ALBUM_ARTIST TAB ALBUM TAB TRACK
 *   TAB DISC TAB YEAR TAB TITLE
 *
 * A song whose time and size match its line is not read again, so later
 * runs take a moment. Music runs it as it opens and shows the progress it
 * prints: "read N" (the songs to read), then "done K" as it goes.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <libavformat/avformat.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_SONGS	4000
#define HEADER		"s22-music-tags 1"

/* As s22-music.c's */
static const char *const audio_ext[] = {
	"mp3", "flac", "ogg", "oga", "opus", "m4a", "m4b", "aac", "alac", "wav",
	"wma", "ape", "wv", "mpc", "mp2", "aif", "aiff", "aifc", "dsf", "dff",
	"tta", "mka", "webm", "spx", "caf", "ac3", "dts", "amr", "mod", "xm",
	"s3m", "it", "mp4", "3gp", "au", "snd", "voc", "w64", "rf64", NULL,
};

struct song {
	char *path;
	long long mtime, size;
	char *line;		/* the cache's line, if it still holds */
};

static struct song songs[MAX_SONGS];
static int nsongs;

static struct old {
	char *path;
	long long mtime, size;
	char *line;
} *old;
static int nold;

static bool is_audio(const char *name)
{
	const char *dot = strrchr(name, '.');

	if (!dot)
		return false;
	for (int i = 0; audio_ext[i]; i++)
		if (!strcasecmp(dot + 1, audio_ext[i]))
			return true;
	return false;
}

static void scan(const char *dir, int depth)
{
	DIR *d = opendir(dir);
	struct dirent *de;

	while (d && (de = readdir(d)) && nsongs < MAX_SONGS) {
		if (de->d_name[0] == '.')
			continue;
		char *path;
		struct stat st;
		if (asprintf(&path, "%s/%s", dir, de->d_name) < 0)
			continue;
		if (stat(path, &st) != 0) {
			free(path);
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			if (depth < 8)
				scan(path, depth + 1);
			free(path);
			continue;
		}
		if (!is_audio(de->d_name) || strchr(path, '\t') || strchr(path, '\n')) {
			free(path);
			continue;
		}
		songs[nsongs++] = (struct song){ path, st.st_mtime, st.st_size, NULL };
	}
	if (d)
		closedir(d);
}

static int by_path(const void *a, const void *b)
{
	return strcmp(((const struct old *)a)->path, ((const struct old *)b)->path);
}

/* The cache as it was: its lines by path */
static void load_old(const char *cache)
{
	FILE *f = fopen(cache, "re");
	char *line = NULL;
	size_t cap = 0;

	if (!f)
		return;
	old = calloc(MAX_SONGS, sizeof(*old));
	if (getline(&line, &cap, f) < 0 || strncmp(line, HEADER "\n", sizeof(HEADER))) {
		fclose(f);
		free(line);
		return;
	}
	while (old && nold < MAX_SONGS && getline(&line, &cap, f) > 0) {
		char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
		if (!t2)
			continue;
		old[nold].line = strdup(line);
		old[nold].path = strndup(line, t1 - line);
		old[nold].mtime = atoll(t1 + 1);
		old[nold].size = atoll(t2 + 1);
		nold++;
	}
	free(line);
	fclose(f);
	qsort(old, nold, sizeof(*old), by_path);
}

/* A tag, tabs and line ends made spaces */
static void put(FILE *f, AVDictionary *m, const char *const *keys)
{
	AVDictionaryEntry *e = NULL;

	for (int i = 0; !e && keys[i]; i++)
		e = av_dict_get(m, keys[i], NULL, 0);
	fputc('\t', f);
	for (const char *p = e ? e->value : ""; *p; p++)
		fputc(*p == '\t' || *p == '\n' || *p == '\r' ? ' ' : *p, f);
}

static char *read_tags(struct song *s)
{
	AVFormatContext *fc = NULL;
	char *line = NULL;
	size_t len = 0;
	FILE *f = open_memstream(&line, &len);

	if (!f)
		return NULL;
	fprintf(f, "%s\t%lld\t%lld", s->path, s->mtime, s->size);
	if (avformat_open_input(&fc, s->path, NULL, NULL) == 0) {
		AVDictionary *m = fc->metadata;
		/* Ogg and Opus keep theirs on the stream */
		if (!av_dict_count(m) && fc->nb_streams)
			m = fc->streams[0]->metadata;
		put(f, m, (const char *const[]){ "artist", NULL });
		put(f, m, (const char *const[]){ "album_artist", "albumartist", "album artist", NULL });
		put(f, m, (const char *const[]){ "album", NULL });
		put(f, m, (const char *const[]){ "track", NULL });
		put(f, m, (const char *const[]){ "disc", NULL });
		put(f, m, (const char *const[]){ "date", "year", NULL });
		put(f, m, (const char *const[]){ "title", NULL });
		avformat_close_input(&fc);
	} else {
		fputs("\t\t\t\t\t\t\t", f);
	}
	fputc('\n', f);
	fclose(f);
	return line;
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: s22-music-tags MUSIC_DIR CACHE\n");
		return 2;
	}
	(void)!nice(10);
	av_log_set_level(AV_LOG_QUIET);
	setvbuf(stdout, NULL, _IOLBF, 0);
	load_old(argv[2]);
	scan(argv[1], 0);

	int todo = 0, done = 0;
	for (int i = 0; i < nsongs; i++) {
		struct old key = { .path = songs[i].path };
		struct old *o = nold ? bsearch(&key, old, nold, sizeof(*old), by_path) : NULL;
		if (o && o->mtime == songs[i].mtime && o->size == songs[i].size)
			songs[i].line = o->line;
		else
			todo++;
	}
	printf("read %d\n", todo);
	/* Nothing new, none gone: the cache stands */
	if (!todo && nsongs == nold)
		return 0;

	char tmp[4200];
	snprintf(tmp, sizeof(tmp), "%s.new", argv[2]);
	char *slash = strrchr(tmp, '/');
	if (slash) {		/* ~/.cache, then ours */
		*slash = 0;
		char *up = strrchr(tmp, '/');
		if (up) {
			*up = 0;
			mkdir(tmp, 0755);
			*up = '/';
		}
		mkdir(tmp, 0755);
		*slash = '/';
	}
	FILE *f = fopen(tmp, "we");
	if (!f) {
		perror(tmp);
		return 1;
	}
	fputs(HEADER "\n", f);
	for (int i = 0; i < nsongs; i++) {
		if (songs[i].line) {
			fputs(songs[i].line, f);
			continue;
		}
		char *line = read_tags(&songs[i]);
		if (line)
			fputs(line, f);
		free(line);
		if (++done % 10 == 0)
			printf("done %d\n", done);
	}
	if (fclose(f) != 0 || rename(tmp, argv[2]) != 0) {
		perror(argv[2]);
		unlink(tmp);
		return 1;
	}
	printf("done %d\n", done);
	return 0;
}
