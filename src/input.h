#pragma once
// File parsing

#include <algorithm>
#include <cassert>
#include <cerrno>  // LineByLineReader
#include <cstdio>  // LineByLineReader io
#include <cstdlib> // free
#include <memory>  // open_file
#include <stdexcept>
#include <string>
#include <system_error> // io errors

#include "types.h"
#include "utils.h"

/******************************************************************************
 * File parsing utils.
 */

// Custom deleters for unique_ptr
struct FreeDeleter {
	void operator() (void * ptr) noexcept { std::free (ptr); }
};
struct FcloseDeleter {
	void operator() (std::FILE * f) noexcept {
		std::fclose (f); // Ignore fclose errors
	}
};

/* Open file with error checking.
 * On error, an exception is thrown.
 * Same arguments as fopen.
 */
inline std::unique_ptr<std::FILE, FcloseDeleter> open_file (string_view pathname, string_view mode) {
	// fopen requires null terminated strings.
	errno = 0;
	std::unique_ptr<std::FILE, FcloseDeleter> file (
	    std::fopen (to_string (pathname).c_str (), to_string (mode).c_str ()));
	if (!file) {
		throw std::system_error (errno, std::system_category ());
	}
	return file;
}

/* LineByLineReader allows to read a file line by line.
 * After construction, the reader has no stored data.
 * The user should call read_next_line to extract a line from the file.
 * The line is stored in an internal buffer, and is valid until the next call to read_next_line.
 */
class LineByLineReader {
private:
	not_null<std::FILE *> input_;

	std::unique_ptr<char, FreeDeleter> current_line_data_{nullptr};
	std::size_t current_line_data_buffer_size_{0};
	std::size_t current_line_size_{0};

	std::size_t lines_read_{0};

public:
	// State: no stored data
	explicit LineByLineReader (not_null<std::FILE *> input) : input_ (input) {}

	/* Read a line from the file.
	 * Returns true if a line was read, false on eof.
	 * Throws an exception on error.
	 */
	bool read_next_line ();

	// Access current line as const
	string_view current_line () const {
		if (current_line_data_) {
			return string_view (current_line_data_.get (), current_line_size_);
		} else {
			throw std::runtime_error ("LineByLineReader: no line data available");
		}
	}

	// Counts from 0
	std::size_t current_line_number () const { return lines_read_ - 1; }

	bool eof () const { return std::feof (input_); }
};

inline bool LineByLineReader::read_next_line () {
	char * buf_ptr = current_line_data_.release ();
	errno = 0;
	auto r = ::getline (&buf_ptr, &current_line_data_buffer_size_, input_);
	auto error_code = errno;
	current_line_data_.reset (buf_ptr);

	if (r >= 0) {
		current_line_size_ = static_cast<std::size_t> (r);
		++lines_read_;
		return true;
	} else {
		if (std::feof (input_)) {
			current_line_data_.reset (); // Clear buffer so that current_line will fail.
			return false;
		} else {
			throw std::system_error (error_code, std::system_category ());
		}
	}
}

/******************************************************************************
 * BED format parsing.
 * TODO factorize by DataType, with a DataType from_bed_interval (start, end) function.
 * TODO support explicit listing of which region goes into what vector index.
 */

inline std::vector<ProcessRegionData<Point>> read_points_from_bed_file (not_null<FILE *> file) {
	std::vector<ProcessRegionData<Point>> regions;
	LineByLineReader reader (file);

	std::vector<Point> current_region_points;
	std::string current_region_name;

	try {
		while (reader.read_next_line ()) {
			const string_view line = trim_ws (reader.current_line ());
			if (starts_with ('#', line)) {
				// Comment or header, ignore
			} else {
				const auto fields = split ('\t', line);
				if (!(fields.size () >= 3)) {
					throw std::runtime_error ("Line must contain at least 3 fields: (region, start, end)");
				}
				const string_view region_name = fields[0];
				const auto interval_start_position = parse_int (fields[1], "interval_position_start");
				const auto interval_end_position = parse_int (fields[2], "interval_position_end");
				if (!(interval_start_position < interval_end_position)) {
					throw std::runtime_error ("interval bounds are invalid");
				}
				// Check is start of a new region
				if (region_name != current_region_name) {
					if (empty (region_name)) {
						throw std::runtime_error ("empty string as a region name");
					}
					if (!empty (current_region_name)) {
						// End current region and store its data
						regions.emplace_back (ProcessRegionData<Point>{
						    current_region_name, SortedVec<Point>::from_unsorted (std::move (current_region_points))});
					}
					current_region_points.clear ();
					current_region_name = to_string (region_name);
				}
				current_region_points.emplace_back ((interval_end_position - interval_start_position) / 2);
			}
		}
		return regions;
	} catch (const std::runtime_error & e) {
		// Add some context to an error.
		throw std::runtime_error (
		    fmt::format ("Parsing BED file at line {}: {}", reader.current_line_number () + 1, e.what ()));
	}
}
