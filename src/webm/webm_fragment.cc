#include <getopt.h>
#include <fcntl.h>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <utility>
#include <memory>
#include <stdexcept>

#include "path.hh"
#include "tokenize.hh"
#include "exception.hh"
#include "file_descriptor.hh"
#include "strict_conversions.hh"

#include "mkvparser/mkvparser.h"
#include "mkvparser/mkvreader.h"
#include "mkvmuxer/mkvmuxer.h"
#include "mkvmuxer/mkvmuxerutil.h"
#include "mkvmuxer/mkvwriter.h"

using namespace std;

void print_usage(const string & program_name)
{
  cerr <<
  "Usage: " << program_name << " [options] <input_segment>\n\n"
  "<input_segment>    input WebM segment to fragment\n\n"
  "Options:\n"
  "--init-segment, -i     output initial segment\n"
  "--media-segment, -m    output media segment in the format of <num>.chk,\n"
  "                       where <num> denotes the segment number\n"
  "--timecode-file, -t    file to store the timecode for the next segment"
  << endl;
}

void create_init_segment(mkvmuxer::MkvWriter * writer,
                         mkvparser::MkvReader * reader,
                         const unique_ptr<mkvparser::Segment> & parser_segment)
{
  /* get Segment Info element */
  auto parser_info = parser_segment->GetInfo();
  if (not parser_info) {
    throw runtime_error("Segment::GetInfo() failed");
  }

  /* get Tracks element */
  auto parser_tracks = parser_segment->GetTracks();
  if (not parser_tracks) {
    throw runtime_error("Segment::GetTracks() failed");
  }

  /* get Tags element */
  auto parser_tags = parser_segment->GetTags();
  if (not parser_tags) {
    throw runtime_error("Segment::GetTags() failed");
  }

  /* create muxer for Segment */
  auto muxer_segment = make_unique<mkvmuxer::Segment>();
  if (not muxer_segment->Init(writer)) {
    throw runtime_error("failed to initialize muxer segment");
  }

  /* write Segment header */
  if (mkvmuxer::WriteID(writer, libwebm::kMkvSegment)) {
    throw runtime_error("WriteID failed while writing Segment header");
  }

  if (mkvmuxer::SerializeInt(writer, mkvmuxer::kEbmlUnknownValue, 8)) {
    throw runtime_error("SerializeInt failed while writing Segment header");
  }

  /* write Segment Info with no duration in particular */
  auto muxer_info = muxer_segment->GetSegmentInfo();
  muxer_info->set_timecode_scale(parser_info->GetTimeCodeScale());
  muxer_info->Write(writer);

  /* simply copy Tracks element */
  long long tracks_start = parser_tracks->m_element_start;
  long tracks_size = narrow_cast<long>(parser_tracks->m_element_size);
  auto tracks_buffer = make_unique<unsigned char[]>(tracks_size);

  if (reader->Read(tracks_start, tracks_size, tracks_buffer.get())) {
    throw runtime_error("failed to read (copy) Tracks element");
  }

  if (writer->Write(tracks_buffer.get(), tracks_size)) {
    throw runtime_error("failed to write (forward) Tracks element");
  }

  /* copy all tags */
  auto muxer_tags = make_unique<mkvmuxer::Tags>();

  for (int i = 0; i < parser_tags->GetTagCount(); ++i) {
    auto parser_tag = parser_tags->GetTag(i);

    for (int j = 0; j < parser_tag->GetSimpleTagCount(); ++j) {
      auto parser_simple_tag = parser_tag->GetSimpleTag(j);
      auto tag_name = parser_simple_tag->GetTagName();
      auto tag_string = parser_simple_tag->GetTagString();

      auto muxer_tag = muxer_tags->AddTag();
      muxer_tag->add_simple_tag(tag_name, tag_string);
    }
  }

  muxer_tags->Write(writer);
}

int get_sequence_number(const string & filepath)
{
  roost::path file_path = roost::path(filepath);
  string filename = roost::rbasename(file_path).string();
  string number_str = split_filename(filename).first;
  return stoi(number_str);
}

long long get_timecode(const string & timecode_file)
{
  FileDescriptor timecode_fd(
      CheckSystemCall("open (" + timecode_file + ")",
                      open(timecode_file.c_str(), O_RDONLY)));

  return stoll(timecode_fd.read());
}

void set_timecode(const string & timecode_file, long long timecode)
{
  FileDescriptor timecode_fd(CheckSystemCall("open (" + timecode_file + ")",
      open(timecode_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644)));

  timecode_fd.write(to_string(timecode));
}

void create_media_segment(
    mkvmuxer::MkvWriter * writer,
    mkvparser::MkvReader * reader,
    const unique_ptr<mkvparser::Segment> & parser_segment,
    const string & media_segment,
    const string & timecode_file)
{
  if (parser_segment->GetCount() != 1) {
    throw runtime_error("input WebM should contain a single Cluster element");
  }

  /* copy Cluster except BlockGroup */
  auto cluster = parser_segment->GetFirst();
  if (not cluster) {
    throw runtime_error("no Cluster element is found");
  }

  const mkvparser::BlockEntry * block_entry;
  if (cluster->GetFirst(block_entry)) {
    throw runtime_error("failed to get the first block of cluster");
  }

  /* get the last SimpleBlock and BlockGroup */
  const mkvparser::BlockEntry * last_simple_block_entry = nullptr;
  const mkvparser::BlockEntry * block_group_entry = nullptr;

  while (block_entry and not block_entry->EOS()) {
    if (block_entry->GetKind() == mkvparser::BlockEntry::kBlockSimple) {
      last_simple_block_entry = block_entry;
    } else if (block_entry->GetKind() == mkvparser::BlockEntry::kBlockGroup) {
      block_group_entry = block_entry;
    }

    if (cluster->GetNext(block_entry, block_entry)) {
      throw runtime_error("failed to get the next block of cluster");
    }
  }

  if (last_simple_block_entry == nullptr) {
    throw runtime_error("no SimpleBlock exists");
  }

  if (block_group_entry == nullptr) {
    throw runtime_error("no BlockGroup exists");
  }

  /* get absolute (correct) timecode ... */
  long long abs_timecode;
  long long rel_timecode = block_group_entry->GetBlock()->GetTimeCode(cluster);

  if (timecode_file.empty()) {
    /* ... from filename */
    int sequence_number = get_sequence_number(media_segment);
    abs_timecode = sequence_number * rel_timecode;
  } else {
    /* ... from timecode file and save the timecode for the next segment */
    abs_timecode = get_timecode(timecode_file);
    set_timecode(timecode_file, abs_timecode + rel_timecode);
  }

  /* calculate sizes */
  auto last_block = last_simple_block_entry->GetBlock();
  long long last_block_end = last_block->m_start + last_block->m_size;
  long long first_block_start = cluster->GetFirstBlockPos();
  long long copy_size = last_block_end - first_block_start;

  long long timecode_size = mkvmuxer::EbmlElementSize(
      libwebm::kMkvTimecode, abs_timecode);
  long long payload_size = copy_size + timecode_size;

  /* manually write Cluster header with the correct payload size */
  if (mkvmuxer::WriteID(writer, libwebm::kMkvCluster)) {
    throw runtime_error("WriteID failed while writing Cluster header");
  }

  if (mkvmuxer::WriteUInt(writer, payload_size)) {
    throw runtime_error("SerializeInt failed while writing Cluster header");
  }

  if (not mkvmuxer::WriteEbmlElement(
          writer, libwebm::kMkvTimecode, abs_timecode)) {
    throw runtime_error("failed to write Timecode");
  }

  /* copy all SimpleBlocks */
  auto cluster_buffer = make_unique<unsigned char[]>(copy_size);

  if (reader->Read(first_block_start, copy_size, cluster_buffer.get())) {
    throw runtime_error("failed to read (copy) Cluster element");
  }

  if (writer->Write(cluster_buffer.get(), copy_size)) {
    throw runtime_error("failed to write (forward) Cluster element");
  }
}

void fragment(const string & input_webm,
              const string & init_segment,
              const string & media_segment,
              const string & timecode_file)
{
  mkvparser::MkvReader reader;
  if (reader.Open(input_webm.c_str())) {
    throw runtime_error("error while opening " + input_webm);
  }

  mkvmuxer::MkvWriter init_writer;
  if (init_segment.size()) {
    if (not init_writer.Open(init_segment.c_str())) {
      throw runtime_error("error while opening " + init_segment);
    }
  }

  mkvmuxer::MkvWriter media_writer;
  if (media_segment.size()) {
    if (not media_writer.Open(media_segment.c_str())) {
      throw runtime_error("error while opening " + media_segment);
    }
  }

  long long pos = 0;

  /* parse EBML header */
  mkvparser::EBMLHeader ebml_header;
  long long ret = ebml_header.Parse(&reader, pos);
  if (ret) {
    throw runtime_error("EBMLHeader::Parse() failed");
  }

  /* write EBML header in init segment */
  if (init_segment.size()) {
    mkvmuxer::WriteEbmlHeader(&init_writer, ebml_header.m_docTypeVersion,
                              ebml_header.m_docType);
  }

  /* parse Segment element */
  mkvparser::Segment * parser_segment_raw;
  ret = mkvparser::Segment::CreateInstance(&reader, pos, parser_segment_raw);
  if (ret) {
    throw runtime_error("Segment::CreateInstance() failed");
  }

  std::unique_ptr<mkvparser::Segment> parser_segment(parser_segment_raw);
  ret = parser_segment->Load();
  if (ret < 0) {
    throw runtime_error("Segment::Load() failed");
  }

  /* write the rest of init segment */
  if (init_segment.size()) {
    create_init_segment(&init_writer, &reader, parser_segment);
  }

  /* write media segment */
  if (media_segment.size()) {
    create_media_segment(&media_writer, &reader, parser_segment,
                         media_segment, timecode_file);
  }
}

int main(int argc, char * argv[])
{
  if (argc < 1) {
    abort();
  }

  string init_segment, media_segment;
  string timecode_file;

  const option cmd_line_opts[] = {
    {"init-segment",  required_argument, nullptr, 'i'},
    {"media-segment", required_argument, nullptr, 'm'},
    {"timecode-file", required_argument, nullptr, 't'},
    { nullptr,        0,                 nullptr,  0 }
  };

  while (true) {
    const int opt = getopt_long(argc, argv, "i:m:t:", cmd_line_opts, nullptr);
    if (opt == -1) {
      break;
    }

    switch (opt) {
    case 'i':
      init_segment = optarg;
      break;
    case 'm':
      media_segment = optarg;
      break;
    case 't':
      timecode_file = optarg;
      break;
    default:
      print_usage(argv[0]);
      return EXIT_FAILURE;
    }
  }

  if (optind != argc - 1) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  string input_segment = argv[optind];

  if (init_segment.empty() and media_segment.empty()) {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  fragment(input_segment, init_segment, media_segment, timecode_file);

  return EXIT_SUCCESS;
}