/*
 *   Copyright (C) 2022 SUSE LLC
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Written by Olaf Kirch <okir@suse.com>
 */

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>
#include <iconv.h>
#include <limits.h>

#include <tss2/tss2_tpm2_types.h>

#include "eventlog.h"
#include "bufparser.h"
#include "util.h"

#define TPM_EVENT_LOG_MAX_ALGOS		64

struct tpm_event_log_reader {
	int			fd;
	unsigned int		tpm_version;

	struct tpm_event_log_tcg2_info {
		uint32_t		platform_class;
		uint8_t			spec_version_major;
		uint8_t			spec_version_minor;
		uint8_t			spec_errata;
		uint8_t			uintn_size;

		tpm_algo_info_t		algorithms[TPM_EVENT_LOG_MAX_ALGOS];
	} tcg2_info;
};


static bool		__tpm_event_parse_tcg2_info(tpm_event_t *ev, struct tpm_event_log_tcg2_info *info);


static void
__read_exactly(int fd, void *vp, unsigned int len)
{
	int n;

	if ((n = read(fd, vp, len)) < 0)
		fatal("unable to read from event log: %m\n");
	if (n != len)
		fatal("short read from event log (premature EOF)\n");
}

static void
__read_u32le(int fd, uint32_t *vp)
{
	__read_exactly(fd, vp, sizeof(*vp));
	*vp = le32toh(*vp);
}

static void
__read_u16le(int fd, uint16_t *vp)
{
	__read_exactly(fd, vp, sizeof(*vp));
	*vp = le16toh(*vp);
}

static bool
__read_u32le_or_eof(int fd, uint32_t *vp)
{
	int n;

	if ((n = read(fd, vp, 4)) < 0)
		fatal("unable to read from event log: %m\n");
	if (n == 0)
		return false;

	if (n != 4)
		fatal("short read from event log (premature EOF)\n");
	*vp = le32toh(*vp);
	return true;
}

static const tpm_algo_info_t *
event_log_get_algo_info(tpm_event_log_reader_t *log, unsigned int algo_id)
{
	const tpm_algo_info_t *algo;

	if (!(algo = digest_by_tpm_alg(algo_id)))
		algo = __digest_by_tpm_alg(algo_id, log->tcg2_info.algorithms, TPM_EVENT_LOG_MAX_ALGOS);
	return algo;
}

tpm_event_log_reader_t *
event_log_open(void)
{
	const char *eventlog_path = "/sys/kernel/security/tpm0/binary_bios_measurements";
	tpm_event_log_reader_t *log;

	log = calloc(1, sizeof(*log));
	log->tpm_version = 1;

	if ((log->fd = open(eventlog_path, O_RDONLY)) < 0)
		fatal("Unable to open TPM eventlog \"%s\"\n", eventlog_path);
	return log;
}

void
event_log_close(tpm_event_log_reader_t *log)
{
	close(log->fd);
	free(log);
}

static void
event_log_read_digest(tpm_event_log_reader_t *log, tpm_evdigest_t *dgst, int tpm_hash_algo_id)
{
	const tpm_algo_info_t *algo;

	if (!(algo = event_log_get_algo_info(log, tpm_hash_algo_id)))
		fatal("Unable to handle event log entry for unknown hash algorithm %u\n", tpm_hash_algo_id);

	__read_exactly(log->fd, dgst->data, algo->digest_size);

	dgst->algo = algo;
	dgst->size = algo->digest_size;
}

static void
event_log_resize_pcrs(tpm_event_t *ev, unsigned int count)
{
	if (count > 32)
		fatal("Bad number of PCRs in TPM event record (%u)\n", count);

	ev->pcr_values = calloc(count, sizeof(tpm_evdigest_t));
	if (ev->pcr_values == NULL)
		fatal("out of memory");
	ev->pcr_count = count;
}

static void
event_log_read_pcrs_tpm1(tpm_event_log_reader_t *log, tpm_event_t *ev)
{
	event_log_resize_pcrs(ev, 1);
	event_log_read_digest(log, &ev->pcr_values[0], TPM2_ALG_SHA1);
}

static void
event_log_read_pcrs_tpm2(tpm_event_log_reader_t *log, tpm_event_t *ev)
{
	uint32_t i, count;

	__read_u32le(log->fd, &count);
	event_log_resize_pcrs(ev, count);

	for (i = 0; i < count; ++i) {
		uint16_t algo_id;

		__read_u16le(log->fd, &algo_id);
		event_log_read_digest(log, &ev->pcr_values[i], algo_id);
	}
}

tpm_event_t *
event_log_read_next(tpm_event_log_reader_t *log)
{
	tpm_event_t *ev;
	uint32_t event_size;

again:
	ev = calloc(1, sizeof(*ev));
	if (!__read_u32le_or_eof(log->fd, &ev->pcr_index)) {
		free(ev);
		return NULL;
	}

	__read_u32le(log->fd, &ev->event_type);

	ev->file_offset = lseek(log->fd, 0, SEEK_CUR);

	if (log->tpm_version == 1) {
		event_log_read_pcrs_tpm1(log, ev);
	} else {
		event_log_read_pcrs_tpm2(log, ev);
	}

	__read_u32le(log->fd, &event_size);
	if (event_size > 8192)
		fatal("Oversized TPM2 event log entry with %u bytes of data\n", event_size);

	ev->event_data = calloc(1, event_size);
	ev->event_size = event_size;
	__read_exactly(log->fd, ev->event_data, event_size);


	if (log->tpm_version == 1 && ev->event_type == TPM2_EVENT_NO_ACTION
	 && !strncmp((char *) ev->event_data, "Spec ID Event03", 16)) {
		printf("Detected TPMv2 event log\n");

		if (!__tpm_event_parse_tcg2_info(ev, &log->tcg2_info))
			fatal("Unable to parse TCG2 magic event header");

		log->tpm_version = log->tcg2_info.spec_version_major;
		free(ev);
		goto again;
	}

	return ev;
}

/*
 * TCGv2 defines a "magic event" record that conveys some additional information
 * on where the log was created, the hash sizes for the algorithms etc.
 */
static bool
__tpm_event_parse_tcg2_info(tpm_event_t *ev, struct tpm_event_log_tcg2_info *info)
{
	buffer_t buf;
	uint32_t i, algo_info_count;

	buffer_init_read(&buf, ev->event_data, ev->event_size);

	/* skip over magic signature string */
	buffer_skip(&buf, 16);

	if (!buffer_get_u32le(&buf, &info->platform_class)
	 || !buffer_get_u8(&buf, &info->spec_version_major)
	 || !buffer_get_u8(&buf, &info->spec_version_minor)
	 || !buffer_get_u8(&buf, &info->spec_errata)
	 || !buffer_get_u8(&buf, &info->uintn_size)
	 || !buffer_get_u32le(&buf, &algo_info_count)
	   )
		return false;

	for (i = 0; i < algo_info_count; ++i) {
		uint16_t algo_id, algo_size;
		const tpm_algo_info_t *wk;

		if (!buffer_get_u16le(&buf, &algo_id)
		 || !buffer_get_u16le(&buf, &algo_size))
			return false;

		if (algo_id > TPM2_ALG_LAST)
			continue;

		if ((wk = digest_by_tpm_alg(algo_id)) == NULL) {
			char fake_name[32];

			snprintf(fake_name, sizeof(fake_name), "TPM2_ALG_%u", algo_id);
			info->algorithms[algo_id].digest_size = algo_size;
			info->algorithms[algo_id].openssl_name = strdup(fake_name);
		} else if (wk->digest_size != algo_size) {
			fprintf(stderr, "Conflicting digest sizes for %s: %u versus %u\n",
					wk->openssl_name, wk->digest_size, algo_size);
		} else
			/* NOP */ ;
	}

	return true;
}

const char *
tpm_event_type_to_string(unsigned int event_type)
{
	static char buffer[16];

	switch (event_type) {
	case TPM2_EVENT_PREBOOT_CERT:
		return "EVENT_PREBOOT_CERT";
	case TPM2_EVENT_POST_CODE:
		return "EVENT_POST_CODE";
	case TPM2_EVENT_UNUSED:
		return "EVENT_UNUSED";
	case TPM2_EVENT_NO_ACTION:
		return "EVENT_NO_ACTION";
	case TPM2_EVENT_SEPARATOR:
		return "EVENT_SEPARATOR";
	case TPM2_EVENT_ACTION:
		return "EVENT_ACTION";
	case TPM2_EVENT_EVENT_TAG:
		return "EVENT_EVENT_TAG";
	case TPM2_EVENT_S_CRTM_CONTENTS:
		return "EVENT_S_CRTM_CONTENTS";
	case TPM2_EVENT_S_CRTM_VERSION:
		return "EVENT_S_CRTM_VERSION";
	case TPM2_EVENT_CPU_MICROCODE:
		return "EVENT_CPU_MICROCODE";
	case TPM2_EVENT_PLATFORM_CONFIG_FLAGS:
		return "EVENT_PLATFORM_CONFIG_FLAGS";
	case TPM2_EVENT_TABLE_OF_DEVICES:
		return "EVENT_TABLE_OF_DEVICES";
	case TPM2_EVENT_COMPACT_HASH:
		return "EVENT_COMPACT_HASH";
	case TPM2_EVENT_IPL:
		return "EVENT_IPL";
	case TPM2_EVENT_IPL_PARTITION_DATA:
		return "EVENT_IPL_PARTITION_DATA";
	case TPM2_EVENT_NONHOST_CODE:
		return "EVENT_NONHOST_CODE";
	case TPM2_EVENT_NONHOST_CONFIG:
		return "EVENT_NONHOST_CONFIG";
	case TPM2_EVENT_NONHOST_INFO:
		return "EVENT_NONHOST_INFO";
	case TPM2_EVENT_OMIT_BOOT_DEVICE_EVENTS:
		return "EVENT_OMIT_BOOT_DEVICE_EVENTS";
	case TPM2_EFI_EVENT_BASE:
		return "EFI_EVENT_BASE";
	case TPM2_EFI_VARIABLE_DRIVER_CONFIG:
		return "EFI_VARIABLE_DRIVER_CONFIG";
	case TPM2_EFI_VARIABLE_BOOT:
		return "EFI_VARIABLE_BOOT";
	case TPM2_EFI_BOOT_SERVICES_APPLICATION:
		return "EFI_BOOT_SERVICES_APPLICATION";
	case TPM2_EFI_BOOT_SERVICES_DRIVER:
		return "EFI_BOOT_SERVICES_DRIVER";
	case TPM2_EFI_RUNTIME_SERVICES_DRIVER:
		return "EFI_RUNTIME_SERVICES_DRIVER";
	case TPM2_EFI_GPT_EVENT:
		return "EFI_GPT_EVENT";
	case TPM2_EFI_ACTION:
		return "EFI_ACTION";
	case TPM2_EFI_PLATFORM_FIRMWARE_BLOB:
		return "EFI_PLATFORM_FIRMWARE_BLOB";
	case TPM2_EFI_HANDOFF_TABLES:
		return "EFI_HANDOFF_TABLES";
	case TPM2_EFI_PLATFORM_FIRMWARE_BLOB2:
		return "EFI_PLATFORM_FIRMWARE_BLOB2";
	case TPM2_EFI_HANDOFF_TABLES2:
		return "EFI_HANDOFF_TABLES2";
	case TPM2_EFI_VARIABLE_BOOT2:
		return "EFI_VARIABLE_BOOT2";
	case TPM2_EFI_HCRTM_EVENT:
		return "EFI_HCRTM_EVENT";
	case TPM2_EFI_VARIABLE_AUTHORITY:
		return "EFI_VARIABLE_AUTHORITY";
	case TPM2_EFI_SPDM_FIRMWARE_BLOB:
		return "EFI_SPDM_FIRMWARE_BLOB";
	case TPM2_EFI_SPDM_FIRMWARE_CONFIG:
		return "EFI_SPDM_FIRMWARE_CONFIG";
	}

	snprintf(buffer, sizeof(buffer), "0x%x", event_type);
	return buffer;
}

const tpm_evdigest_t *
tpm_event_get_digest(const tpm_event_t *ev, const char *algo_name)
{
	const tpm_algo_info_t *algo_info;
	unsigned int i;

	if ((algo_info = digest_by_name(algo_name)) < 0)
		fatal("Unknown algo name \"%s\"\n", algo_name);

	for (i = 0; i < ev->pcr_count; ++i) {
		const tpm_evdigest_t *md = &ev->pcr_values[i];

		if (md->algo == algo_info)
			return md;
	}

	return NULL;
}

void
tpm_event_print(tpm_event_t *ev)
{
	__tpm_event_print(ev, (void (*)(const char *, ...)) printf);
}

void
__tpm_event_print(tpm_event_t *ev, tpm_event_bit_printer *print_fn)
{
	tpm_parsed_event_t *parsed;
	unsigned int i;

	print_fn("%05lx: event type=%s pcr=%d digests=%d data=%u bytes\n",
			ev->file_offset,
			tpm_event_type_to_string(ev->event_type),
			ev->pcr_index, ev->pcr_count, ev->event_size);

	parsed = tpm_event_parse(ev);
	if (parsed)
		tpm_parsed_event_print(parsed, print_fn);

	for (i = 0; i < ev->pcr_count; ++i) {
		const tpm_evdigest_t *d = &ev->pcr_values[i];

		print_fn("  %-10s %s\n", d->algo->openssl_name, digest_print_value(d));
	}

	print_fn("  Data:\n");
	hexdump(ev->event_data, ev->event_size, print_fn, 8);
}

static tpm_parsed_event_t *
tpm_parsed_event_new(unsigned int event_type)
{
	tpm_parsed_event_t *parsed;

	parsed = calloc(1, sizeof(*parsed));
	parsed->event_type = event_type;
	return parsed;
}

static void
tpm_parsed_event_free(tpm_parsed_event_t *parsed)
{
	if (parsed->destroy)
		parsed->destroy(parsed);
	memset(parsed, 0, sizeof(*parsed));
	free(parsed);
}

void
tpm_parsed_event_print(tpm_parsed_event_t *parsed, tpm_event_bit_printer *print_fn)
{
	if (parsed && parsed->print)
		parsed->print(parsed, print_fn);
}

buffer_t *
tpm_parsed_event_rebuild(tpm_parsed_event_t *parsed, const void *raw_data, unsigned int raw_data_len)
{
	if (parsed && parsed->rebuild)
		return parsed->rebuild(parsed, raw_data, raw_data_len);

	return NULL;
}

const char *
tpm_event_decode_uuid(const unsigned char *data)
{
	static char uuid[64];
	uint32_t w0;
	uint16_t hw0, hw1;

	w0 = le32toh(((uint32_t *) data)[0]);
	hw0 = le32toh(((uint16_t *) data)[2]);
	hw1 = le32toh(((uint16_t *) data)[3]);
	snprintf(uuid, sizeof(uuid), "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			w0, hw0, hw1,
			data[8], data[9],
			data[10], data[11], data[12],
			data[13], data[14], data[15]
			);
	return uuid;
}

/*
 * Process EFI_VARIABLE events
 */
static void
__tpm_event_efi_variable_destroy(tpm_parsed_event_t *parsed)
{
}

static void
__tpm_event_efi_variable_print(tpm_parsed_event_t *parsed, tpm_event_bit_printer *print_fn)
{
	print_fn("  --> EFI variable %s: %u bytes of data\n",
			tpm_efi_variable_event_extract_full_varname(parsed),
			parsed->efi_variable_event.len);
}

static bool
__tpm_event_marshal_efi_variable(buffer_t *bp, const tpm_parsed_event_t *parsed, const void *raw_data, unsigned int raw_data_len)
{
	unsigned int var_len, name_len;

	if (!buffer_put(bp, parsed->efi_variable_event.variable_guid, sizeof(parsed->efi_variable_event.variable_guid)))
		return false;

	var_len = strlen(parsed->efi_variable_event.variable_name);
	if (!buffer_put_u64le(bp, var_len)
	 || !buffer_put_u64le(bp, raw_data_len)
	 || !buffer_put_utf16le(bp, parsed->efi_variable_event.variable_name, &name_len)
	 || !buffer_put(bp, raw_data, raw_data_len))
		return false;

	if (name_len != 2 * var_len)
		return false;

	return true;
}

static buffer_t *
__tpm_event_efi_variable_rebuild(const tpm_parsed_event_t *parsed, const void *raw_data, unsigned int raw_data_len)
{
	buffer_t *bp;

	/* The marshal buffer needs to hold
	 * GUID, 2 * UINT64, plus the UTF16 encoding of the variable name, plus the raw efivar value */
	bp = buffer_alloc_write(16 + 8 + 8 +
			+ 2 * strlen(parsed->efi_variable_event.variable_name)
			+ raw_data_len);

	if (!__tpm_event_marshal_efi_variable(bp, parsed, raw_data, raw_data_len)) {
		debug("Failed to marshal EFI variable %s\n", parsed->efi_variable_event.variable_name);
		buffer_free(bp);
		return NULL;
	}

	return bp;
}

static bool
__tpm_event_parse_efi_variable(tpm_event_t *ev, tpm_parsed_event_t *parsed, buffer_t *bp)
{
	uint64_t name_len, data_len;

	parsed->destroy = __tpm_event_efi_variable_destroy;
	parsed->print = __tpm_event_efi_variable_print;
	parsed->rebuild = __tpm_event_efi_variable_rebuild;

	if (!buffer_get(bp, parsed->efi_variable_event.variable_guid, sizeof(parsed->efi_variable_event.variable_guid)))
		return false;

	if (!buffer_get_u64le(bp, &name_len) || !buffer_get_u64le(bp, &data_len))
		return false;

	if (!(parsed->efi_variable_event.variable_name = buffer_get_utf16le(bp, name_len)))
		return false;

	parsed->efi_variable_event.data = malloc(data_len);
	if (!buffer_get(bp, parsed->efi_variable_event.data, data_len))
		return false;
	parsed->efi_variable_event.len = data_len;

	return parsed;
}

const char *
tpm_efi_variable_event_extract_full_varname(tpm_parsed_event_t *parsed)
{
	static char varname[256];

	snprintf(varname, sizeof(varname), "%s-%s", 
			parsed->efi_variable_event.variable_name,
			tpm_event_decode_uuid(parsed->efi_variable_event.variable_guid));
	return varname;
}

static void
__tpm_event_efi_bsa_destroy(tpm_parsed_event_t *parsed)
{
	__tpm_event_efi_device_path_destroy(&parsed->efi_bsa_event.device_path);
}

static void
__tpm_event_efi_bsa_print(tpm_parsed_event_t *parsed, tpm_event_bit_printer *print_fn)
{
#if 0
	print_fn("BSA image loc=%Lx", (unsigned long long) parsed->efi_bsa_event.image_location);
	print_fn(" len=%Lx", (unsigned long long) parsed->efi_bsa_event.image_length);
	print_fn(" lt-addr=%Lx", (unsigned long long) parsed->efi_bsa_event.image_lt_address);
	print_fn("\n");
#endif

	print_fn("Boot Service Application; device path:\n");
	__tpm_event_efi_device_path_print(&parsed->efi_bsa_event.device_path, print_fn);
}

static bool
__tpm_event_parse_efi_bsa(tpm_event_t *ev, tpm_parsed_event_t *parsed, buffer_t *bp)
{
	size_t device_path_len;
	buffer_t path_buf;

	parsed->destroy = __tpm_event_efi_bsa_destroy;
	parsed->print = __tpm_event_efi_bsa_print;

	if (!buffer_get_u64le(bp, &parsed->efi_bsa_event.image_location)
	 || !buffer_get_size(bp, &parsed->efi_bsa_event.image_length)
	 || !buffer_get_size(bp, &parsed->efi_bsa_event.image_lt_address)
	 || !buffer_get_size(bp, &device_path_len)
	 || !buffer_get_buffer(bp, device_path_len, &path_buf))
		return false;

	if (!__tpm_event_parse_efi_device_path(&parsed->efi_bsa_event.device_path, &path_buf))
		return false;

	return true;
}

bool
tpm_efi_bsa_event_extract_location(tpm_parsed_event_t *parsed, char **dev_ret, char **path_ret)
{
	const struct efi_device_path *efi_path;
	const struct efi_device_path_item *item;
	unsigned int i;

	if (parsed->event_type != TPM2_EFI_BOOT_SERVICES_APPLICATION)
		return false;

	drop_string(path_ret);

	efi_path = &parsed->efi_bsa_event.device_path;
	for (i = 0, item = efi_path->entries; i < efi_path->count; ++i, ++item) {
		char pathbuf[PATH_MAX];
		const char *uuid, *filepath;

		if ((uuid = __tpm_event_efi_device_path_item_harddisk_uuid(item)) != NULL) {
			char *dev_path;

			snprintf(pathbuf, sizeof(pathbuf), "/dev/disk/by-partuuid/%s", uuid);
			if ((dev_path = realpath(pathbuf, NULL)) == NULL) {
				fprintf(stderr, "Error: cannot find device for partition with uuid %s\n", uuid);
				return false;
			}

			drop_string(dev_ret);
			*dev_ret = dev_path;
		}

		if ((filepath = __tpm_event_efi_device_path_item_file_path(item)) != NULL) {
			assign_string(path_ret, filepath);
		}
	}

	return *dev_ret && *path_ret;
}

/*
 * Handle IPL events, which grub2 uses to hide its stuff in
 */
static void
__tpm_event_grub_file_destroy(tpm_parsed_event_t *parsed)
{
	drop_string(&parsed->grub_file.device);
	drop_string(&parsed->grub_file.path);
}

static void
__tpm_event_grub_file_print(tpm_parsed_event_t *parsed, tpm_event_bit_printer *print_fn)
{
	if (parsed->grub_file.device == NULL)
		print_fn("  grub2 file load from %s\n", parsed->grub_file.path);
	else
		print_fn("  grub2 file load from (%s)%s\n", parsed->grub_file.device, parsed->grub_file.path);
}

static buffer_t *
__tpm_event_grub_file_rebuild(const tpm_parsed_event_t *parsed, const void *raw_data, unsigned int raw_data_len)
{
	return NULL;
}

/*
 * For files residing on the EFI partition, grub usually formats these as
 * (hdX,gptY)/EFI/BOOT/some.file
 * Once it has determined the final root device, the device part will be
 * omitted (eg for kernel and initrd).
 */
static bool
__tpm_event_grub_file_event_parse(tpm_event_t *ev, tpm_parsed_event_t *parsed, const char *value)
{
	if (value[0] == '/') {
		parsed->grub_file.device = NULL;
		parsed->grub_file.path = strdup(value);
	} else if (value[0] == '(') {
		char *copy = strdup(value);
		char *path;

		if ((path = strchr(copy, ')')) == NULL) {
			free(copy);
			return false;
		}

		*path++ = '\0';

		parsed->grub_file.device = strdup(copy + 1);
		parsed->grub_file.path = strdup(path);
		free(copy);
	} else {
		return false;
	}

	parsed->event_subtype = GRUB_EVENT_FILE;
	parsed->destroy = __tpm_event_grub_file_destroy;
	parsed->print = __tpm_event_grub_file_print;
	parsed->rebuild = __tpm_event_grub_file_rebuild;

	return true;
}

static void
__tpm_event_grub_command_destroy(tpm_parsed_event_t *parsed)
{
	int argc;

	drop_string(&parsed->grub_command.string);
	for (argc = 0; argc < GRUB_COMMAND_ARGV_MAX; argc++)
		drop_string(&parsed->grub_command.argv[argc]);
}

static void
__tpm_event_grub_command_print(tpm_parsed_event_t *parsed, tpm_event_bit_printer *print_fn)
{
	if (parsed->event_subtype == GRUB_EVENT_COMMAND)
		print_fn("  grub2 command \"%s\"\n", parsed->grub_command.string);
	else
		print_fn("  grub2 kernel cmdline \"%s\"\n", parsed->grub_command.string);
}

/*
 * This event holds stuff like
 *  grub_cmd: ....
 *  kernel_cmdline: ...
 */
static bool
__tpm_event_grub_command_event_parse(tpm_event_t *ev, tpm_parsed_event_t *parsed, const char *value)
{
	unsigned int wordlen;
	char *copy, *keyword, *arg, *s, cc;
	int argc;

	/* clear argv */
	memset(&parsed->grub_command, 0, sizeof(parsed->grub_command));

	for (wordlen = 0; (cc = value[wordlen]) && (isalpha(cc) || cc == '_'); ++wordlen)
		;

	if (value[wordlen] != ':' || value[wordlen + 1] != ' ')
		return false;

	copy = strdup(value);
	copy[wordlen++] = '\0';
	copy[wordlen++] = '\0';

	keyword = copy;
	arg = copy + wordlen;

	if (!strcmp(keyword, "grub_cmd")) {
		parsed->event_subtype = GRUB_EVENT_COMMAND;
	} else
	if (!strcmp(keyword, "kernel_cmdline")) {
		parsed->event_subtype = GRUB_EVENT_KERNEL_CMDLINE;
	} else {
		free(copy);
		return false;
	}

	parsed->grub_command.string = strdup(arg);
	for (argc = 0, s = strtok(arg, " \t"); s && argc < GRUB_COMMAND_ARGV_MAX - 1; s = strtok(NULL, " \t")) {
		parsed->grub_command.argv[argc++] = strdup(s);
		parsed->grub_command.argv[argc] = NULL;
	}

	parsed->destroy = __tpm_event_grub_command_destroy;
	parsed->print = __tpm_event_grub_command_print;

	free(copy);
	return true;
}


static bool
__tpm_event_parse_ipl(tpm_event_t *ev, tpm_parsed_event_t *parsed, buffer_t *bp)
{
	const char *value = (const char *) ev->event_data;
	unsigned int len = ev->event_size;

	if (len == 0 || *value == '\0')
		return false;

	/* ATM, grub2 records the string including its trailing NUL byte */
	if (value[len - 1] != '\0')
		return false;

	if (ev->pcr_index == 8)
		return __tpm_event_grub_command_event_parse(ev, parsed, value);

	if (ev->pcr_index == 9)
		return __tpm_event_grub_file_event_parse(ev, parsed, value);

	return false;
}

static bool
__tpm_event_parse(tpm_event_t *ev, tpm_parsed_event_t *parsed)
{
	buffer_t buf;

	buffer_init_read(&buf, ev->event_data, ev->event_size);

	switch (ev->event_type) {
	case TPM2_EVENT_IPL:
		return __tpm_event_parse_ipl(ev, parsed, &buf);

	case TPM2_EFI_VARIABLE_AUTHORITY:
	case TPM2_EFI_VARIABLE_BOOT:
	case TPM2_EFI_VARIABLE_DRIVER_CONFIG:
		return __tpm_event_parse_efi_variable(ev, parsed, &buf);

	case TPM2_EFI_BOOT_SERVICES_APPLICATION:
		return __tpm_event_parse_efi_bsa(ev, parsed, &buf);
	}

	return false;
}

tpm_parsed_event_t *
tpm_event_parse(tpm_event_t *ev)
{
	if (!ev->__parsed) {
		tpm_parsed_event_t *parsed;

		parsed = tpm_parsed_event_new(ev->event_type);
		if (__tpm_event_parse(ev, parsed))
			ev->__parsed = parsed;
		else
			tpm_parsed_event_free(parsed);
	}

	return ev->__parsed;
}
