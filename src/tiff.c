/*
  Slideviewer, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2020  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "common.h"

#ifndef IS_SERVER
#include <glad/glad.h>
#endif

#define TIFF_VERBOSE 0

#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>

#include "lz4.h"

#include "tiff.h"

u32 get_tiff_field_size(u16 data_type) {
	u32 size = 0;
	switch(data_type) {
		default:
			printf("Warning: encountered a TIFF field with an unrecognized data type (%d)\n", data_type);
			break;
		case TIFF_UINT8: case TIFF_INT8: case TIFF_ASCII: case TIFF_UNDEFINED: size = 1; break;
		case TIFF_UINT16: case TIFF_INT16:                                     size = 2; break;
		case TIFF_UINT32: case TIFF_INT32: case TIFF_IFD: case TIFF_FLOAT:     size = 4; break;
		case TIFF_RATIONAL:	case TIFF_SRATIONAL:                               size = 8; break; // note: actually 2x4
		case TIFF_DOUBLE: case TIFF_UINT64: case TIFF_INT64: case TIFF_IFD8:   size = 8; break;
	}
	return size;
}

void maybe_swap_tiff_field(void* field, u16 data_type, bool32 is_big_endian) {
	if (is_big_endian) {
		u32 field_size = get_tiff_field_size(data_type);
		if (field_size > 1) {
			// Some fields consist of two smaller field (RATIONAL, SRATIONAL), their components need to be swapped individually
			i32 sub_count = (data_type == TIFF_RATIONAL || data_type == TIFF_SRATIONAL) ? 2 : 1;
			u8* pos = (u8*) field;
			for (i32 i = 0; i < sub_count; ++i, pos += field_size) {
				switch(field_size) {
					case 2: *(u16*)pos = bswap_16(*(u16*)pos); break;
					case 4: *(u32*)pos = bswap_32(*(u32*)pos); break;
					case 8: *(u64*)pos = bswap_64(*(u64*)pos); break;
					default: ASSERT(!"This field size should not exist");
				}
			}
		}
	}
}

const char* get_tiff_tag_name(u32 tag) {
	const char* result = "unrecognized tag";
	switch(tag) {
		case TIFF_TAG_NEW_SUBFILE_TYPE: result = "NewSubfileType"; break;
		case TIFF_TAG_IMAGE_WIDTH: result = "ImageWidth"; break;
		case TIFF_TAG_IMAGE_LENGTH: result = "ImageLength"; break;
		case TIFF_TAG_BITS_PER_SAMPLE: result = "BitsPerSample"; break;
		case TIFF_TAG_COMPRESSION: result = "Compression"; break;
		case TIFF_TAG_PHOTOMETRIC_INTERPRETATION: result = "PhotometricInterpretation"; break;
		case TIFF_TAG_IMAGE_DESCRIPTION: result = "ImageDescription"; break;
		case TIFF_TAG_STRIP_OFFSETS: result = "StripOffsets"; break;
		case TIFF_TAG_ORIENTATION: result = "Orientation"; break;
		case TIFF_TAG_SAMPLES_PER_PIXEL: result = "SamplesPerPixel"; break;
		case TIFF_TAG_ROWS_PER_STRIP: result = "RowsPerStrip"; break;
		case TIFF_TAG_STRIP_BYTE_COUNTS: result = "StripByteCounts"; break;
		case TIFF_TAG_PLANAR_CONFIGURATION: result = "PlanarConfiguration"; break;
		case TIFF_TAG_SOFTWARE: result = "Software"; break;
		case TIFF_TAG_TILE_WIDTH: result = "TileWidth"; break;
		case TIFF_TAG_TILE_LENGTH: result = "TileLength"; break;
		case TIFF_TAG_TILE_OFFSETS: result = "TileOffsets"; break;
		case TIFF_TAG_TILE_BYTE_COUNTS: result = "TileByteCounts"; break;
		case TIFF_TAG_JPEG_TABLES: result = "JPEGTables"; break;
		case TIFF_TAG_YCBCRSUBSAMPLING: result = "YCbCrSubSampling"; break;
		case TIFF_TAG_REFERENCEBLACKWHITE: result = "ReferenceBlackWhite"; break;
		default: break;
	}
	return result;
}

u64 file_read_at_offset(void* dest, FILE* fp, u64 offset, u64 num_bytes) {
	fpos_t prev_read_pos = {0}; // NOTE: fpos_t may be a struct!
	int ret = fgetpos64(fp, &prev_read_pos); // for restoring the file position later
	ASSERT(ret == 0); (void)ret;

	fseeko64(fp, offset, SEEK_SET);
	u64 result = fread(dest, num_bytes, 1, fp);

	ret = fsetpos64(fp, &prev_read_pos); // restore previous file position
	ASSERT(ret == 0); (void)ret;

	return result;
}

char* tiff_read_field_ascii(tiff_t* tiff, tiff_tag_t* tag) {
	size_t description_length = tag->data_count;
	char* result = (char*) calloc(ATLEAST(8, description_length + 1), 1);
	if (tag->data_is_offset) {
		file_read_at_offset(result, tiff->fp, tag->offset, tag->data_count);
	} else {
		memcpy(result, tag->data, description_length);
	}
	return result;
}

static inline void* tiff_read_field_undefined(tiff_t* tiff, tiff_tag_t* tag) {
	return (void*) tiff_read_field_ascii(tiff, tag);
}

// Read integer values in a TIFF tag (either 8, 16, 32, or 64 bits wide) + convert them to little-endian u64 if needed
u64* tiff_read_field_integers(tiff_t* tiff, tiff_tag_t* tag) {
	u64* integers = NULL;

	if (tag->data_is_offset) {
		u64 bytesize = get_tiff_field_size(tag->data_type);
		void* temp_integers = calloc(bytesize, tag->data_count);
		if (file_read_at_offset(temp_integers, tiff->fp, tag->offset, tag->data_count * bytesize) != 1) {
			free(temp_integers);
			return NULL; // failed
		}

		if (bytesize == 8) {
			// the numbers are already 64-bit, no need to widen
			integers = (u64*) temp_integers;
			if (tiff->is_big_endian) {
				for (i32 i = 0; i < tag->data_count; ++i) {
					integers[i] = bswap_64(integers[i]);
				}
			}
		} else {
			// offsets are 32-bit or less -> widen to 64-bit offsets
			integers = (u64*) malloc(tag->data_count * sizeof(u64));
			switch(bytesize) {
				case 4: {
					for (i32 i = 0; i < tag->data_count; ++i) {
						integers[i] = maybe_swap_32(((u32*) temp_integers)[i], tiff->is_big_endian);
					}
				} break;
				case 2: {
					for (i32 i = 0; i < tag->data_count; ++i) {
						integers[i] = maybe_swap_16(((u16*) temp_integers)[i], tiff->is_big_endian);
					}
				} break;
				case 1: {
					for (i32 i = 0; i < tag->data_count; ++i) {
						integers[i] = ((u8*) temp_integers)[i];
					}
				} break;
				default: {
					free(temp_integers);
					free(integers);
					return NULL; // failed (other bytesizes than the above shouldn't exist)
				}
			}
			free(temp_integers);
		}
		// all done!

	} else {
		// data is inlined
		integers = (u64*) malloc(sizeof(u64));
		integers[0] = tag->data_u64;
	}

	return integers;
}


tiff_rational_t* tiff_read_field_rationals(tiff_t* tiff, tiff_tag_t* tag) {
	tiff_rational_t* rationals = (tiff_rational_t*) calloc(ATLEAST(8, tag->data_count * sizeof(tiff_rational_t)), 1);

	if (tag->data_is_offset) {
		file_read_at_offset(rationals, tiff->fp, tag->offset, tag->data_count * sizeof(tiff_rational_t));
	} else {
		// data is inlined
		rationals = (tiff_rational_t*) malloc(sizeof(u64));
		rationals[0] = *(tiff_rational_t*) tag->data_u64;
	}

	if (tiff->is_big_endian) {
		for (i32 i = 0; i < tag->data_count; ++i) {
			tiff_rational_t* rational = rationals + i;
			rational->a = bswap_32(rational->a);
			rational->b = bswap_32(rational->b);
		}
	}

	return rationals;
}

bool32 tiff_read_ifd(tiff_t* tiff, tiff_ifd_t* ifd, u64* next_ifd_offset) {
	bool32 is_bigtiff = tiff->is_bigtiff;
	bool32 is_big_endian = tiff->is_big_endian;

	// By default, assume RGB color space.
	// (although TIFF files are always required to specify this in the PhotometricInterpretation tag)
	ifd->color_space = TIFF_PHOTOMETRIC_RGB;

	// Set the file position to the start of the IFD
	if (!(next_ifd_offset != NULL && fseeko64(tiff->fp, *next_ifd_offset, SEEK_SET) == 0)) {
		return false; // failed
	}

	u64 tag_count = 0;
	u64 tag_count_num_bytes = is_bigtiff ? 8 : 2;
	if (fread(&tag_count, tag_count_num_bytes, 1, tiff->fp) != 1) return false;
	if (is_big_endian) {
		tag_count = is_bigtiff ? bswap_64(tag_count) : bswap_16(tag_count);
	}

	// Read the tags
	u64 tag_size = is_bigtiff ? 20 : 12;
	u64 bytes_to_read = tag_count * tag_size;
	u8* raw_tags = (u8*) malloc(bytes_to_read);
	if (fread(raw_tags, bytes_to_read, 1, tiff->fp) != 1) {
		free(raw_tags);
		return false; // failed
	}

	// Restructure the fields so we don't have to worry about the memory layout, endianness, etc
	tiff_tag_t* tags = (tiff_tag_t*) calloc(sizeof(tiff_tag_t) * tag_count, 1);
	for (i32 i = 0; i < tag_count; ++i) {
		tiff_tag_t* tag = tags + i;
		if (is_bigtiff) {
			raw_bigtiff_tag_t* raw = (raw_bigtiff_tag_t*)raw_tags + i;
			tag->code = maybe_swap_16(raw->code, is_big_endian);
			tag->data_type = maybe_swap_16(raw->data_type, is_big_endian);
			tag->data_count = maybe_swap_64(raw->data_count, is_big_endian);

			u32 field_size = get_tiff_field_size(tag->data_type);
			u64 data_size = field_size * tag->data_count;
			if (data_size <= 8) {
				// Data fits in the tag so it is inlined
				memcpy(tag->data, raw->data, 8);
				maybe_swap_tiff_field(tag->data, tag->data_type, is_big_endian);
				tag->data_is_offset = false;
			} else {
				// Data doesn't fit in the tag itself, so it's an offset
				tag->offset = maybe_swap_64(raw->offset, is_big_endian);
				tag->data_is_offset = true;
			}
		} else {
			// Standard TIFF
			raw_tiff_tag_t* raw = (raw_tiff_tag_t*)raw_tags + i;
			tag->code = maybe_swap_16(raw->code, is_big_endian);
			tag->data_type = maybe_swap_16(raw->data_type, is_big_endian);
			tag->data_count = maybe_swap_32(raw->data_count, is_big_endian);

			u32 field_size = get_tiff_field_size(tag->data_type);
			u64 data_size = field_size * tag->data_count;
			if (data_size <= 4) {
				// Data fits in the tag so it is inlined
				memcpy(tag->data, raw->data, 4);
				maybe_swap_tiff_field(tag->data, tag->data_type, is_big_endian);
				tag->data_is_offset = false;
			} else {
				// Data doesn't fit in the tag itself, so it's an offset
				tag->offset = maybe_swap_32(raw->offset, is_big_endian);
				tag->data_is_offset = true;
			}
		}
	}
	free(raw_tags);

	// Read and interpret the entries in the IFD
	for (i32 tag_index = 0; tag_index < tag_count; ++tag_index) {
		tiff_tag_t* tag = tags + tag_index;
#if TIFF_VERBOSE
		printf("tag %2d: %30s - code=%d, data_type=%2d, count=%5llu, offset=%llu\n",
		       tag_index, get_tiff_tag_name(tag->code), tag->code, tag->data_type, tag->data_count, tag->offset);
#endif
		switch(tag->code) {
			case TIFF_TAG_NEW_SUBFILE_TYPE: {
				ifd->tiff_subfiletype = tag->data_u32;
			} break;
			// Note: the data type of many tags (E.g. ImageWidth) can actually be either SHORT or LONG,
			// but because we already converted the byte order to native (=little-endian) with enough
			// padding in the tag struct, we can get away with treating them as if they are always LONG.
			case TIFF_TAG_IMAGE_WIDTH: {
				ifd->image_width = tag->data_u32;
			} break;
			case TIFF_TAG_IMAGE_LENGTH: {
				ifd->image_height = tag->data_u32;
			} break;
			case TIFF_TAG_BITS_PER_SAMPLE: {
#if TIFF_VERBOSE
				// TODO: Fix this for regular TIFF
				if (!tag->data_is_offset) {
					for (i32 i = 0; i < tag->data_count; ++i) {
						u16 bits = *(u16*)&tag->data[i*2];
						printf("   channel %d: BitsPerSample=%d\n", i, bits); // expected to be 8
					}
				}
#endif
			} break;
			case TIFF_TAG_COMPRESSION: {
				ifd->compression = tag->data_u16;
			} break;
			case TIFF_TAG_PHOTOMETRIC_INTERPRETATION: {
				ifd->color_space = tag->data_u16;
			} break;
			case TIFF_TAG_IMAGE_DESCRIPTION: {
				ifd->image_description = tiff_read_field_ascii(tiff, tag);
				ifd->image_description_length = tag->data_count;
#if TIFF_VERBOSE
				printf("%.500s\n", ifd->image_description);
#endif
			} break;
			case TIFF_TAG_TILE_WIDTH: {
				ifd->tile_width = tag->data_u32;
			} break;
			case TIFF_TAG_TILE_LENGTH: {
				ifd->tile_height = tag->data_u32;
			} break;
			case TIFF_TAG_TILE_OFFSETS: {
				// TODO: to be sure, need check PlanarConfiguration==1 to check how to interpret the data count?
				ifd->tile_count = tag->data_count;
				ifd->tile_offsets = tiff_read_field_integers(tiff, tag);
				if (ifd->tile_offsets == NULL) {
					free(tags);
					return false; // failed
				}
			} break;
			case TIFF_TAG_TILE_BYTE_COUNTS: {
				// Note: is it OK to assume that the TileByteCounts will always come after the TileOffsets?
				if (tag->data_count != ifd->tile_count) {
					ASSERT(tag->data_count != 0);
					printf("Error: mismatch in the TIFF tile count reported by TileByteCounts and TileOffsets tags\n");
					free(tags);
					return false; // failed;
				}
				ifd->tile_byte_counts = tiff_read_field_integers(tiff, tag);
				if (ifd->tile_byte_counts == NULL) {
					free(tags);
					return false; // failed
				}
			} break;
			case TIFF_TAG_JPEG_TABLES: {
				ifd->jpeg_tables = (u8*) tiff_read_field_undefined(tiff, tag);
				ifd->jpeg_tables_length = tag->data_count;
			} break;
			case TIFF_TAG_YCBCRSUBSAMPLING: {
				// https://www.awaresystems.be/imaging/tiff/tifftags/ycbcrsubsampling.html
				ifd->chroma_subsampling_horizontal = *(u16*)&tag->data[0];
				ifd->chroma_subsampling_vertical = *(u16*)&tag->data[2];
#if TIFF_VERBOSE
				printf("   YCbCrSubsampleHoriz = %d, YCbCrSubsampleVert = %d\n", ifd->chroma_subsampling_horizontal, ifd->chroma_subsampling_vertical);
#endif

			} break;
			case TIFF_TAG_REFERENCEBLACKWHITE: {
				ifd->reference_black_white_rational_count = tag->data_count;
				ifd->reference_black_white = tiff_read_field_rationals(tiff, tag); //TODO: free, add to serialized format
				if (ifd->reference_black_white == NULL) {
					free(tags);
					return false; // failed
				}
#if TIFF_VERBOSE
				for (i32 i = 0; i < tag->data_count; ++i) {
					tiff_rational_t* reference_black_white = ifd->reference_black_white + i;
					printf("    [%d] = %d / %d\n", i, reference_black_white->a, reference_black_white->b);
				}
#endif
			} break;
			default: {
			} break;
		}


	}

	free(tags);

	if (ifd->tile_width > 0) {
		ifd->width_in_tiles = (ifd->image_width + ifd->tile_width - 1) / ifd->tile_width;
	}
	if (ifd->tile_height > 0) {
		ifd->height_in_tiles = (ifd->image_height + ifd->tile_height - 1) / ifd->tile_height;
	}

	// Try to deduce what type of image this is (level, macro, or label).
	// Unfortunately this does not seem to be very consistently specified in the TIFF files, so in part we have to guess.
	if (ifd->image_description) {
		if (strncmp(ifd->image_description, "Macro", 5) == 0) {
			ifd->subimage_type = TIFF_MACRO_SUBIMAGE;
			tiff->macro_image = ifd;
			tiff->macro_image_index = ifd->ifd_index;
		} else if (strncmp(ifd->image_description, "Label", 5) == 0) {
			ifd->subimage_type = TIFF_LABEL_SUBIMAGE;
			tiff->label_image = ifd;
			tiff->label_image_index = ifd->ifd_index;
		} else if (strncmp(ifd->image_description, "level", 5) == 0) {
			ifd->subimage_type = TIFF_LEVEL_SUBIMAGE;
		}
	}
	// Guess that it must be a level image if it's not explicitly said to be something else
	if (ifd->subimage_type == TIFF_UNKNOWN_SUBIMAGE /*0*/ && ifd->tile_width > 0) {
		if (ifd->ifd_index == 0 /* main image */ || ifd->tiff_subfiletype & TIFF_FILETYPE_REDUCEDIMAGE) {
			ifd->subimage_type = TIFF_LEVEL_SUBIMAGE;
		}
	}



	// Read the next IFD
	if (fread(next_ifd_offset, tiff->bytesize_of_offsets, 1, tiff->fp) != 1) return false;
#if TIFF_VERBOSE
	printf("next ifd offset = %lld\n", *next_ifd_offset);
#endif
	return true; // success
}

bool32 open_tiff_file(tiff_t* tiff, const char* filename) {
#if TIFF_VERBOSE
	printf("Opening TIFF file %s\n", filename);
#endif
	int ret = 0; (void)ret; // for checking return codes from fgetpos, fsetpos, etc
	FILE* fp = fopen64(filename, "rb");
	bool32 success = false;
	if (fp) {
		tiff->fp = fp;
		struct stat st;
		if (fstat(fileno(fp), &st) == 0) {
			i64 filesize = st.st_size;
			if (filesize > 8) {
				// read the 8-byte TIFF header / 16-byte BigTIFF header
				tiff_header_t tiff_header = {};
				if (fread(&tiff_header, sizeof(tiff_header_t) /*16*/, 1, fp) != 1) goto fail;
				bool32 is_big_endian;
				switch(tiff_header.byte_order_indication) {
					case TIFF_BIG_ENDIAN: is_big_endian = true; break;
					case TIFF_LITTLE_ENDIAN: is_big_endian = false; break;
					default: goto fail;
				}
				tiff->is_big_endian = is_big_endian;
				u16 filetype = maybe_swap_16(tiff_header.filetype, is_big_endian);
				bool32 is_bigtiff;
				switch(filetype) {
					case 0x2A: is_bigtiff = false; break;
					case 0x2B: is_bigtiff = true; break;
					default: goto fail;
				}
				tiff->is_bigtiff = is_bigtiff;
				u32 bytesize_of_offsets;
				u64 next_ifd_offset = 0;
				if (is_bigtiff) {
					bytesize_of_offsets = maybe_swap_16(tiff_header.bigtiff.offset_size, is_big_endian);
					if (bytesize_of_offsets != 8) goto fail;
					if (tiff_header.bigtiff.always_zero != 0) goto fail;
					next_ifd_offset = maybe_swap_64(tiff_header.bigtiff.first_ifd_offset, is_big_endian);
				} else {
					bytesize_of_offsets = 4;
					next_ifd_offset = maybe_swap_32(tiff_header.tiff.first_ifd_offset, is_big_endian);
				}
				ASSERT((bytesize_of_offsets == 4 && !is_bigtiff) || (bytesize_of_offsets == 8 && is_bigtiff));
				tiff->bytesize_of_offsets = bytesize_of_offsets;

				// Read and process the IFDs
				while (next_ifd_offset != 0) {
#if TIFF_VERBOSE
					printf("Reading IFD #%llu\n", tiff->ifd_count);
#endif
					tiff_ifd_t ifd = { .ifd_index = tiff->ifd_count };
					if (!tiff_read_ifd(tiff, &ifd, &next_ifd_offset)) goto fail;
					sb_push(tiff->ifds, ifd);
					tiff->ifd_count += 1;
				}

				// TODO: make more robust
				// Assume the first IFD is the main image, and also level 0
				tiff->main_image = tiff->ifds;
				tiff->main_image_index = 0;
				tiff->level_images = tiff->main_image;
				tiff->level_image_index = 0;

				// TODO: make more robust
				u64 level_counter = 0;
				for (i32 i = 0; i < tiff->ifd_count; ++i) {
					tiff_ifd_t* ifd = tiff->ifds + i;
					if (ifd->subimage_type == TIFF_LEVEL_SUBIMAGE) ++level_counter;
				}
				tiff->level_count = level_counter;

				// TODO: make more robust
				tiff->mpp_x = tiff->mpp_y = 0.25f;
				float um_per_pixel = 0.25f;
				for (i32 i = 0; i < tiff->level_count; ++i) {
					tiff_ifd_t* ifd = tiff->level_images + i;
					// TODO: allow other tile sizes?
					ASSERT(ifd->tile_width == 512);
					ASSERT(ifd->tile_height == 512);
					ifd->um_per_pixel_x = um_per_pixel;
					ifd->um_per_pixel_y = um_per_pixel;
					ifd->x_tile_side_in_um = ifd->um_per_pixel_x * (float)ifd->tile_width;
					ifd->y_tile_side_in_um = ifd->um_per_pixel_y * (float)ifd->tile_height;

					um_per_pixel *= 2.0f; // downsample, so at higher levels there are more pixels per micrometer
				}


				success = true;

			}

		}
		// TODO: better error handling than this crap
		fail:;
		// Note: we need async i/o in the worker threads...
		// so for now we close and reopen the file using platform-native APIs to make that possible.
		fclose(fp);
		tiff->fp = NULL;

#if !IS_SERVER
		// TODO: make async I/O platform agnostic
		// TODO: set FILE_FLAG_NO_BUFFERING for maximum performance (but: need to align read requests to page size...)
		// http://vec3.ca/using-win32-asynchronous-io/
		tiff->win32_file_handle = CreateFileA(filename, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
		                                                 FILE_ATTRIBUTE_NORMAL | /*FILE_FLAG_SEQUENTIAL_SCAN |*/
		                                                 /*FILE_FLAG_NO_BUFFERING |*/ FILE_FLAG_OVERLAPPED,
		                                                 NULL);
#endif


	}
	if (!success) {
		// could not open file
	}
	return success;
}




void push_size(push_buffer_t* buffer, u8* data, u64 size) {
	if (buffer->used_size + size > buffer->capacity) {
		printf("push_size(): buffer overflow\n");
		exit(1);
	}
	memcpy(buffer->data + buffer->used_size, data, size);
	buffer->used_size += size;
}

void push_block(push_buffer_t* buffer, u32 block_type, u32 index, u64 block_length) {
	serial_block_t block = { .block_type = block_type, .index = index, .length = block_length };
	push_size(buffer, (u8*)&block, sizeof(block));
}

#define INCLUDE_IMAGE_DESCRIPTION 1

push_buffer_t* tiff_serialize(tiff_t* tiff, push_buffer_t* buffer) {

	u64 total_size = 0;

	// block: general TIFF header / meta
	total_size += sizeof(serial_block_t);
	tiff_serial_header_t serial_header = (tiff_serial_header_t){
			.filesize = tiff->filesize,
			.ifd_count = tiff->ifd_count,
			.main_image_index = tiff->main_image_index,
			.macro_image_index = tiff->macro_image_index,
			.label_image_index = tiff->label_image_index,
			.level_count = tiff->level_count,
			.level_image_index = tiff->level_image_index,
			.bytesize_of_offsets = tiff->bytesize_of_offsets,
			.is_bigtiff = tiff->is_bigtiff,
			.is_big_endian = tiff->is_big_endian,
			.mpp_x = tiff->mpp_x,
			.mpp_y = tiff->mpp_y,
	};
	total_size += sizeof(serial_header);

	// block: IFD's
	total_size += sizeof(serial_block_t);
	u64 serial_ifds_block_size = tiff->ifd_count * sizeof(tiff_serial_ifd_t);
	tiff_serial_ifd_t* serial_ifds = (tiff_serial_ifd_t*) alloca(serial_ifds_block_size);
	for (i32 i = 0; i < tiff->ifd_count; ++i) {
		tiff_ifd_t* ifd = tiff->ifds + i;
		tiff_serial_ifd_t* serial_ifd = serial_ifds + i;
		*serial_ifd = (tiff_serial_ifd_t) {
			.image_width = ifd->image_width,
			.image_height = ifd->image_height,
			.tile_width = ifd->tile_width,
			.tile_height = ifd->tile_height,
			.tile_count = ifd->tile_count,
			.image_description_length = ifd->image_description_length,
			.jpeg_tables_length = ifd->jpeg_tables_length,
			.compression = ifd->compression,
			.color_space = ifd->color_space,
			.level_magnification = ifd->level_magnification,
			.width_in_tiles = ifd->width_in_tiles,
			.height_in_tiles = ifd->height_in_tiles,
			.um_per_pixel_x = ifd->um_per_pixel_x,
			.um_per_pixel_y = ifd->um_per_pixel_y,
			.x_tile_side_in_um = ifd->x_tile_side_in_um,
			.y_tile_side_in_um = ifd->y_tile_side_in_um,
			.chroma_subsampling_horizontal = ifd->chroma_subsampling_horizontal,
			.chroma_subsampling_vertical = ifd->chroma_subsampling_vertical,
			.subimage_type = ifd->subimage_type,
		};
#if INCLUDE_IMAGE_DESCRIPTION
		total_size += ifd->image_description_length;
#endif
		total_size += ifd->jpeg_tables_length;
		total_size += ifd->tile_count * sizeof(ifd->tile_offsets[0]);
		total_size += ifd->tile_count * sizeof(ifd->tile_byte_counts[0]);
	}
	total_size += tiff->ifd_count * sizeof(tiff_serial_ifd_t);

	// blocks: need separate blocks for each IFD's image descriptions, tile offsets, tile byte counts, jpeg tables
#if INCLUDE_IMAGE_DESCRIPTION
	total_size += tiff->ifd_count * sizeof(serial_block_t);
#endif
	total_size += tiff->ifd_count * sizeof(serial_block_t);
	total_size += tiff->ifd_count * sizeof(serial_block_t);
	total_size += tiff->ifd_count * sizeof(serial_block_t);

	// block: terminator (end of stream marker)
	total_size += sizeof(serial_block_t);

	// Now we know the total size of our uncompressed payload (although if compressed we'll need to rewrite the headers)
	char http_headers[4096];
	snprintf(http_headers, sizeof(http_headers),
	         "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-type: application/octet-stream\r\nContent-length: %-16llu\r\n\r\n",
	         total_size);
	u64 http_headers_size = strlen(http_headers);

	// Hack: we prepend the HTTP headers to the 'normal' buffer, so that we can immediately send everything
	// in one go once we are done serializing.
	buffer->raw_memory = (u8*) malloc(http_headers_size + total_size);
	memcpy(buffer->raw_memory, http_headers, http_headers_size);
//	((char*)buffer->raw_memory)[http_headers_size] = '\0';

	buffer->data = buffer->raw_memory + http_headers_size;
	buffer->used_size = 0;
	buffer->capacity = total_size;

	push_block(buffer, SERIAL_BLOCK_TIFF_HEADER_AND_META, 0,  sizeof(serial_block_t));
	push_size(buffer, (u8*)&serial_header, sizeof(serial_header));

	push_block(buffer, SERIAL_BLOCK_TIFF_IFDS, 0, serial_ifds_block_size);
	push_size(buffer, (u8*)serial_ifds, serial_ifds_block_size);

	for (i32 i = 0; i < tiff->ifd_count; ++i) {
		tiff_ifd_t* ifd = tiff->ifds + i;
#if INCLUDE_IMAGE_DESCRIPTION
		push_block(buffer, SERIAL_BLOCK_TIFF_IMAGE_DESCRIPTION, i, ifd->image_description_length);
		push_size(buffer, (u8*)ifd->image_description, ifd->image_description_length);
#endif
		u64 tile_offsets_size = ifd->tile_count * sizeof(ifd->tile_offsets[0]);
		push_block(buffer, SERIAL_BLOCK_TIFF_TILE_OFFSETS, i, tile_offsets_size);
		push_size(buffer, (u8*)ifd->tile_offsets, tile_offsets_size);

		u64 tile_byte_counts_size = ifd->tile_count * sizeof(ifd->tile_byte_counts[0]);
		push_block(buffer, SERIAL_BLOCK_TIFF_TILE_BYTE_COUNTS, i, tile_byte_counts_size);
		push_size(buffer, (u8*)ifd->tile_byte_counts, tile_byte_counts_size);

		push_block(buffer, SERIAL_BLOCK_TIFF_JPEG_TABLES, i, ifd->jpeg_tables_length);
		push_size(buffer, ifd->jpeg_tables, ifd->jpeg_tables_length);

	}

	push_block(buffer, SERIAL_BLOCK_TERMINATOR, 0, 0);

//	printf("buffer has %llu used bytes, out of %llu capacity\n", buffer->used_size, buffer->capacity);

	// Additional compression step
#if 1
	i32 compression_size_bound = LZ4_COMPRESSBOUND(total_size);
	u8* compression_buffer = (u8*) malloc(compression_size_bound);
	ASSERT(buffer->used_size == total_size);
	i32 compressed_size = LZ4_compress_default((char*)buffer->data, (char*)compression_buffer, buffer->used_size, compression_size_bound);
	if (compressed_size > 0) {
		// success! We can replace the buffer contents with the compressed data
		buffer->used_size = 0;
		push_block(buffer, SERIAL_BLOCK_LZ4_COMPRESSED_DATA, total_size, compressed_size);
		push_size(buffer, compression_buffer, compressed_size);

		// rewrite the HTTP headers at the start, the Content-Length now isn't correct
		snprintf(http_headers, sizeof(http_headers),
		         "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-type: application/octet-stream\r\nContent-length: %-16llu\r\n\r\n",
		         total_size);
		ASSERT(strlen(http_headers) == http_headers_size); // We should be able to assume this because of the padding spaces for the Content-length header field.
		memcpy(buffer->raw_memory, http_headers, http_headers_size);
	}
#endif

	return buffer;

}

u8* pop_from_buffer(u8** pos, i64 size, i64* bytes_left) {
	if (size > *bytes_left) {
		printf("pop_from_buffer(): buffer empty\n");
		return NULL;
	}
	u8* old_pos = *pos;
	*pos += size;
	*bytes_left -= size;
	return old_pos;
}

serial_block_t* pop_block_from_buffer(u8** pos, i64* bytes_left) {
	return (serial_block_t*) pop_from_buffer(pos, sizeof(serial_block_t), bytes_left);
}

i64 find_end_of_http_headers(u8* str, u64 len) {
	static const char crlfcrlf[] = "\r\n\r\n";
	u32 search_key = *(u32*)crlfcrlf;
	i64 result = 0;
	for (i64 offset = 0; offset < len - 4; ++offset) {
		u8* pos = str + offset;
		u32 check = *(u32*)pos;
		if (check == search_key) {
			result = offset + 4;
			break;
		}
	}
	return result;
}



bool32 tiff_deserialize(tiff_t* tiff, u8* buffer, u64 buffer_size) {
	i64 bytes_left = (i64)buffer_size;
	u8* pos = buffer;
	u8* data = NULL;
	serial_block_t* block = NULL;
	bool32 success = false;
	u8* decompressed_buffer = NULL;

	if (0) {
		failed:
		if (decompressed_buffer) free(decompressed_buffer);
		return false;
	}


#define POP_DATA(size) do {if (!(data = pop_from_buffer(&pos, (size), &bytes_left))) goto failed; } while(0)
#define POP_BLOCK() do{if (!(block = pop_block_from_buffer(&pos, &bytes_left))) goto failed; } while(0)


	i64 content_offset = find_end_of_http_headers(buffer, buffer_size);
	i64 content_length = buffer_size - content_offset;
	POP_DATA(content_offset);

	// block: general TIFF header / meta
	POP_BLOCK();

	if (block->block_type == SERIAL_BLOCK_LZ4_COMPRESSED_DATA) {
		// compressed LZ4 stream
		i32 compressed_size = (i32)block->length;
		i32 decompressed_size = (i32)block->index; // used as general purpose field here..
		ASSERT(block->length < INT32_MAX);
		ASSERT(block->index < INT32_MAX);
		POP_DATA(compressed_size);
		decompressed_buffer = (u8*) malloc(decompressed_size);
		i32 bytes_decompressed = LZ4_decompress_safe((char*)data, (char*)decompressed_buffer, compressed_size, decompressed_size);
		if (bytes_decompressed > 0) {
			if (bytes_decompressed != decompressed_size) {
				printf("LZ4_decompress_safe() decompressed %d bytes, however the expected size was %d\n", bytes_decompressed, decompressed_size);
			} else {
				// success, switch over to the uncompressed buffer!
				pos = decompressed_buffer;
				bytes_left = bytes_decompressed;
				POP_BLOCK(); // now pointing to the uncompressed data stream
			}

		} else {
			printf("LZ4_decompress_safe() failed (return value %d)\n", bytes_decompressed);
		}
	}

	if (block->block_type != SERIAL_BLOCK_TIFF_HEADER_AND_META) goto failed;

	POP_DATA(sizeof(tiff_serial_header_t));
	tiff_serial_header_t* serial_header = (tiff_serial_header_t*) data;
	*tiff = (tiff_t) {};
	tiff->is_remote = 0; // set later
	tiff->location = (network_location_t){}; // set later
	tiff->fp = NULL;
#if !IS_SERVER
	tiff->win32_file_handle = NULL;
#endif
	tiff->filesize = serial_header->filesize;
	tiff->bytesize_of_offsets = serial_header->bytesize_of_offsets;
	tiff->ifd_count = serial_header->ifd_count;
	tiff->ifds = NULL; // set later
	tiff->main_image = NULL; // set later
	tiff->main_image_index = serial_header->main_image_index;
	tiff->macro_image = NULL; // set later
	tiff->macro_image_index = serial_header->macro_image_index;
	tiff->label_image = NULL; // set later
	tiff->label_image_index = serial_header->label_image_index;
	tiff->level_count = serial_header->level_count;
	tiff->level_images = NULL; // set later
	tiff->level_image_index = 0; // set later
	tiff->is_bigtiff = serial_header->is_bigtiff;
	tiff->is_big_endian = serial_header->is_big_endian;
	tiff->mpp_x = serial_header->mpp_x;
	tiff->mpp_y = serial_header->mpp_y;

	// block: IFD's
	POP_BLOCK();
	if (block->block_type != SERIAL_BLOCK_TIFF_IFDS) goto failed;
	u64 serial_ifds_block_size = tiff->ifd_count * sizeof(tiff_serial_ifd_t);
	if (block->length != serial_ifds_block_size) goto failed;

	POP_DATA(serial_ifds_block_size);
	tiff_serial_ifd_t* serial_ifds = (tiff_serial_ifd_t*) data;

	// TODO: maybe not use a stretchy_buffer here?
	tiff->ifds = (tiff_ifd_t*) calloc(1, sizeof(tiff_ifd_t) * tiff->ifd_count); // allocate space for the IFD's
	memset(tiff->ifds, 0, tiff->ifd_count * sizeof(tiff_ifd_t));

	for (i32 i = 0; i < tiff->ifd_count; ++i) {
		tiff_ifd_t* ifd = tiff->ifds + i;
		tiff_serial_ifd_t* serial_ifd = serial_ifds + i;
		*ifd = (tiff_ifd_t) {};
		ifd->ifd_index = i;
		ifd->image_width = serial_ifd->image_width;
		ifd->image_height = serial_ifd->image_height;
		ifd->tile_width = serial_ifd->tile_width;
		ifd->tile_height = serial_ifd->tile_height;
		ifd->tile_count = serial_ifd->tile_count;
		ifd->tile_offsets = NULL; // set later
		ifd->tile_byte_counts = NULL; // set later
		ifd->image_description = NULL; // set later
		ifd->image_description_length = serial_ifd->image_description_length;
		ifd->jpeg_tables = NULL; // set later
		ifd->jpeg_tables_length = serial_ifd->jpeg_tables_length;
		ifd->compression = serial_ifd->compression;
		ifd->color_space = serial_ifd->color_space;
		ifd->subimage_type = serial_ifd->subimage_type;
		ifd->level_magnification = serial_ifd->level_magnification;
		ifd->width_in_tiles = serial_ifd->width_in_tiles;
		ifd->height_in_tiles = serial_ifd->height_in_tiles;
		ifd->um_per_pixel_x = serial_ifd->um_per_pixel_x;
		ifd->um_per_pixel_y = serial_ifd->um_per_pixel_y;
		ifd->x_tile_side_in_um = serial_ifd->x_tile_side_in_um;
		ifd->y_tile_side_in_um = serial_ifd->y_tile_side_in_um;
		ifd->chroma_subsampling_horizontal = serial_ifd->chroma_subsampling_horizontal;
		ifd->chroma_subsampling_vertical = serial_ifd->chroma_subsampling_vertical;
		ifd->reference_black_white_rational_count = 0; // unused for now
		ifd->reference_black_white = NULL; // unused for now
	}

	DUMMY_STATEMENT; // for placing a debug breakpoint

	// The number of remaining blocks is unspecified.
	// We are expecting at least byte offsets, tile offsets and JPEG tables for each IFD
	bool32 reached_end = false;
	while (!reached_end) {

		POP_BLOCK();
		if (block->length > 0) {
			POP_DATA(block->length);
		}
		u8* block_content = data;
		// TODO: Need to think about this: are block index's (if present) always referring an IFD index, though? Or are there other use cases?
		if (block->index >= tiff->ifd_count) {
			printf("tiff_deserialize(): found block referencing a non-existent IFD\n");
			goto failed;
		}
		tiff_ifd_t* referenced_ifd = tiff->ifds + block->index;


		switch (block->block_type) {
			case SERIAL_BLOCK_TIFF_IMAGE_DESCRIPTION: {
				if (referenced_ifd->image_description) {
					printf("tiff_deserialize(): IFD %u already has an image description\n", block->index);
					goto failed;
				}
				referenced_ifd->image_description = (char*) malloc(block->length + 1);
				memcpy(referenced_ifd->image_description, block_content, block->length);
				referenced_ifd->image_description[block->length] = '\0';
				referenced_ifd->image_description_length = block->length;
			} break;
			case SERIAL_BLOCK_TIFF_TILE_OFFSETS: {
				if (referenced_ifd->tile_offsets) {
					printf("tiff_deserialize(): IFD %u already has tile offsets\n", block->index);
					goto failed;
				}
				referenced_ifd->tile_offsets = (u64*) malloc(block->length);
				memcpy(referenced_ifd->tile_offsets, block_content, block->length);
			} break;
			case SERIAL_BLOCK_TIFF_TILE_BYTE_COUNTS: {
				if (referenced_ifd->tile_byte_counts) {
					printf("tiff_deserialize(): IFD %u already has tile byte counts\n", block->index);
					goto failed;
				}
				referenced_ifd->tile_byte_counts = (u64*) malloc(block->length);
				memcpy(referenced_ifd->tile_byte_counts, block_content, block->length);
			} break;
			case SERIAL_BLOCK_TIFF_JPEG_TABLES: {
				if (referenced_ifd->jpeg_tables) {
					printf("tiff_deserialize(): IFD %u already has JPEG tables\n", block->index);
					goto failed;
				}
				referenced_ifd->jpeg_tables = (u8*) malloc(block->length);
				memcpy(referenced_ifd->jpeg_tables, block_content, block->length);
				referenced_ifd->jpeg_tables[block->length] = 0;
				referenced_ifd->jpeg_tables_length = block->length;
			} break;
			case SERIAL_BLOCK_TERMINATOR: {
				// Reached the end
				printf("tiff_deserialize(): found a terminator block\n");
				reached_end = true;
				break;
			} break;
		}
	}
	if (reached_end) {
		success = true;
		printf("tiff_deserialize(): bytes_left = %lld, content length = %lld, buffer size = %llu\n", bytes_left, content_length, buffer_size);
	}

	if (decompressed_buffer != NULL) free(decompressed_buffer);


	// make some remaining assumptions
	tiff->main_image = 	tiff->ifds + tiff->main_image_index;
	tiff->macro_image = tiff->ifds + tiff->macro_image_index; // TODO: might not exist??
	tiff->level_images = tiff->ifds + tiff->level_image_index; // TODO: might not exist??

	// todo: flag empty tiles so they don't need to be loaded

	return success;


#undef POP_DATA
#undef POP_BLOCK

}

void tiff_destroy(tiff_t* tiff) {
	if (tiff->fp) {
		fclose(tiff->fp);
		tiff->fp = NULL;
	}
#if !IS_SERVER
	if (tiff->win32_file_handle) {
		CloseHandle(tiff->win32_file_handle);
	}
#endif

	for (i32 i = 0; i < tiff->ifd_count; ++i) {
		tiff_ifd_t* ifd = tiff->ifds + i;
		if (ifd->tile_offsets) free(ifd->tile_offsets);
		if (ifd->tile_byte_counts) free(ifd->tile_byte_counts);
		if (ifd->image_description) free(ifd->image_description);
		if (ifd->jpeg_tables) free(ifd->jpeg_tables);
		if (ifd->reference_black_white) free(ifd->reference_black_white);
	}
	// TODO: fix this, choose either stretchy_buffer or regular malloc, not both...
	if (tiff->is_remote) {
		free(tiff->ifds);
	} else {
		sb_free(tiff->ifds);
	}
	memset(tiff, 0, sizeof(*tiff));
}
