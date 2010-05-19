/* This file is part of Clementine.

   Clementine is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Clementine is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Clementine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "gstenginepipeline.h"
#include "gstequalizer.h"
#include "gstengine.h"

#include <QDebug>

const int GstEnginePipeline::kGstStateTimeoutNanosecs = 10000000;
const int GstEnginePipeline::kFaderFudgeMsec = 2000;

GstEnginePipeline::GstEnginePipeline(GstEngine* engine)
  : QObject(NULL),
    engine_(engine),
    valid_(false),
    sink_(GstEngine::kAutoSink),
    forwards_buffers_(false),
    volume_percent_(100),
    volume_modifier_(1.0),
    fader_(NULL),
    pipeline_(NULL),
    uridecodebin_(NULL),
    audiobin_(NULL),
    audioconvert_(NULL),
    equalizer_(NULL),
    volume_(NULL),
    audioscale_(NULL),
    audiosink_(NULL)
{
}

void GstEnginePipeline::set_output_device(const QString &sink, const QString &device) {
  sink_ = sink;
  device_ = device;
}

bool GstEnginePipeline::StopUriDecodeBin(gpointer bin) {
  gst_element_set_state(GST_ELEMENT(bin), GST_STATE_NULL);
  return false; // So it doesn't get called again
}

bool GstEnginePipeline::ReplaceDecodeBin(const QUrl& url) {
  GstElement* new_bin = engine_->CreateElement("uridecodebin");
  if (!new_bin) return false;

  // Destroy the old one, if any
  if (uridecodebin_) {
    gst_bin_remove(GST_BIN(pipeline_), uridecodebin_);

    // Set its state to NULL later in the main thread
    g_idle_add(GSourceFunc(StopUriDecodeBin), uridecodebin_);
  }

  uridecodebin_ = new_bin;
  gst_bin_add(GST_BIN(pipeline_), uridecodebin_);

  g_object_set(G_OBJECT(uridecodebin_), "uri", url.toEncoded().constData(), NULL);
  g_signal_connect(G_OBJECT(uridecodebin_), "pad-added", G_CALLBACK(NewPadCallback), this);
  g_signal_connect(G_OBJECT(uridecodebin_), "drained", G_CALLBACK(SourceDrainedCallback), this);

  return true;
}

bool GstEnginePipeline::Init(const QUrl &url) {
  pipeline_ = gst_pipeline_new("pipeline");
  url_ = url;

  // Here we create all the parts of the gstreamer pipeline - from the source
  // to the sink.  The parts of the pipeline are split up into bins:
  //   uri decode bin -> audio bin
  // The uri decode bin is a gstreamer builtin that automatically picks the
  // right type of source and decoder for the URI.
  // The audio bin gets created here and contains:
  //   audioconvert -> equalizer -> volume -> audioscale -> audioconvert ->
  //   audiosink

  // Decode bin
  if (!ReplaceDecodeBin(url)) return false;

  // Audio bin
  audiobin_ = gst_bin_new("audiobin");
  gst_bin_add(GST_BIN(pipeline_), audiobin_);

  if (!(audiosink_ = engine_->CreateElement(sink_, audiobin_)))
    return false;

  if (GstEngine::DoesThisSinkSupportChangingTheOutputDeviceToAUserEditableString(sink_) && !device_.isEmpty())
    g_object_set(G_OBJECT(audiosink_), "device", device_.toUtf8().constData(), NULL);

  equalizer_ = GST_ELEMENT(gst_equalizer_new());
  gst_bin_add(GST_BIN(audiobin_), equalizer_);

  if (!(audioconvert_ = engine_->CreateElement("audioconvert", audiobin_))) { return false; }
  if (!(volume_ = engine_->CreateElement("volume", audiobin_))) { return false; }
  if (!(audioscale_ = engine_->CreateElement("audioresample", audiobin_))) { return false; }

  GstPad* pad = gst_element_get_pad(audioconvert_, "sink");
  gst_element_add_pad(audiobin_, gst_ghost_pad_new("sink", pad));
  gst_object_unref(pad);

  // Add a data probe on the src pad of the audioconvert element for our scope.
  // We do it here because we want pre-equalized and pre-volume samples
  // so that our visualization are not affected by them
  pad = gst_element_get_pad(audioconvert_, "src");
  gst_pad_add_buffer_probe(pad, G_CALLBACK(HandoffCallback), this);
  gst_object_unref (pad);

  // Ensure we get the right type out of audioconvert for our scope
  GstCaps* caps = gst_caps_new_simple ("audio/x-raw-int",
      "width", G_TYPE_INT, 16,
      "signed", G_TYPE_BOOLEAN, true,
      NULL);
  gst_element_link_filtered(audioconvert_, equalizer_, caps);
  gst_caps_unref(caps);

  // Add an extra audioconvert at the end as osxaudiosink supports only one format.
  GstElement* convert = engine_->CreateElement("audioconvert", audiobin_, "FFFUUUU");
  if (!convert) { return false; }
  gst_element_link_many(equalizer_, volume_,
                        audioscale_, convert, audiosink_, NULL);

  gst_bus_set_sync_handler(gst_pipeline_get_bus(GST_PIPELINE(pipeline_)), BusCallbackSync, this);
  bus_cb_id_ = gst_bus_add_watch(gst_pipeline_get_bus(GST_PIPELINE(pipeline_)), BusCallback, this);

  return true;
}

GstEnginePipeline::~GstEnginePipeline() {
  if (pipeline_) {
    gst_bus_set_sync_handler(gst_pipeline_get_bus(GST_PIPELINE(pipeline_)), NULL, NULL);
    g_source_remove(bus_cb_id_);
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(GST_OBJECT(pipeline_));
  }
}



gboolean GstEnginePipeline::BusCallback(GstBus*, GstMessage* msg, gpointer self) {
  GstEnginePipeline* instance = reinterpret_cast<GstEnginePipeline*>(self);

  switch ( GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR:
      instance->ErrorMessageReceived(msg);
      break;

    case GST_MESSAGE_TAG:
      instance->TagMessageReceived(msg);
      break;

    default:
      break;
  }
  return GST_BUS_DROP;
}

GstBusSyncReply GstEnginePipeline::BusCallbackSync(GstBus*, GstMessage* msg, gpointer self) {
  GstEnginePipeline* instance = reinterpret_cast<GstEnginePipeline*>(self);
  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
      emit instance->EndOfStreamReached(false);
      break;

    case GST_MESSAGE_TAG:
      instance->TagMessageReceived(msg);
      break;

    case GST_MESSAGE_ERROR:
      instance->ErrorMessageReceived(msg);
      break;

    default:
      break;
  }
  return GST_BUS_PASS;
}

void GstEnginePipeline::ErrorMessageReceived(GstMessage* msg) {
  GError* error;
  gchar* debugs;

  gst_message_parse_error(msg, &error, &debugs);
  QString message = QString::fromLocal8Bit(error->message);

  g_error_free(error);
  free(debugs);

  emit Error(message);
}

void GstEnginePipeline::TagMessageReceived(GstMessage* msg) {
  GstTagList* taglist = NULL;
  gst_message_parse_tag(msg, &taglist);

  Engine::SimpleMetaBundle bundle;
  bundle.title = ParseTag(taglist, GST_TAG_TITLE);
  bundle.artist = ParseTag(taglist, GST_TAG_ARTIST);
  bundle.comment = ParseTag(taglist, GST_TAG_COMMENT);
  bundle.album = ParseTag(taglist, GST_TAG_ALBUM);

  gst_tag_list_free(taglist);

  if (!bundle.title.isEmpty() || !bundle.artist.isEmpty() ||
      !bundle.comment.isEmpty() || !bundle.album.isEmpty())
    emit MetadataFound(bundle);
}

QString GstEnginePipeline::ParseTag(GstTagList* list, const char* tag) const {
  gchar* data = NULL;
  bool success = gst_tag_list_get_string(list, tag, &data);

  QString ret;
  if (success && data) {
    ret = QString::fromUtf8(data);
    g_free(data);
  }
  return ret.trimmed();
}


void GstEnginePipeline::NewPadCallback(GstElement*, GstPad* pad, gpointer self) {
  GstEnginePipeline* instance = reinterpret_cast<GstEnginePipeline*>(self);
  GstPad* const audiopad = gst_element_get_pad(instance->audiobin_, "sink");

  if (GST_PAD_IS_LINKED(audiopad)) {
    qDebug() << "audiopad is already linked. Unlinking old pad." ;
    gst_pad_unlink(audiopad, GST_PAD_PEER(audiopad));
  }

  gst_pad_link(pad, audiopad);

  gst_object_unref(audiopad);
}


bool GstEnginePipeline::HandoffCallback(GstPad*, GstBuffer* buf, gpointer self) {
  GstEnginePipeline* instance = reinterpret_cast<GstEnginePipeline*>(self);

  if (instance->forwards_buffers_) {
    gst_buffer_ref(buf);
    emit instance->BufferFound(buf);
  }

  return true;
}

void GstEnginePipeline::EventCallback(GstPad*, GstEvent* event, gpointer self) {
  GstEnginePipeline* instance = reinterpret_cast<GstEnginePipeline*>(self);

  switch(event->type) {
    case GST_EVENT_EOS:
      emit instance->EndOfStreamReached(false);
      break;

    default:
      break;
  }
}

void GstEnginePipeline::SourceDrainedCallback(GstURIDecodeBin* bin, gpointer self) {
  GstEnginePipeline* instance = reinterpret_cast<GstEnginePipeline*>(self);

  if (instance->next_url_.isValid()) {
    instance->ReplaceDecodeBin(instance->next_url_);
    gst_element_set_state(instance->uridecodebin_, GST_STATE_PLAYING);

    instance->url_ = instance->next_url_;
    instance->next_url_ = QUrl();

    // This just tells the UI that we've moved on to the next song
    emit instance->EndOfStreamReached(true);
  }
}

qint64 GstEnginePipeline::position() const {
  GstFormat fmt = GST_FORMAT_TIME;
  gint64 value = 0;
  gst_element_query_position(pipeline_, &fmt, &value);

  return value;
}


qint64 GstEnginePipeline::length() const {
  GstFormat fmt = GST_FORMAT_TIME;
  gint64 value = 0;
  gst_element_query_duration(pipeline_,  &fmt, &value);

  return value;
}


GstState GstEnginePipeline::state() const {
  GstState s, sp;
  if (gst_element_get_state(pipeline_, &s, &sp, kGstStateTimeoutNanosecs) ==
      GST_STATE_CHANGE_FAILURE)
    return GST_STATE_NULL;

  return s;
}

bool GstEnginePipeline::SetState(GstState state) {
  return gst_element_set_state(pipeline_, state) != GST_STATE_CHANGE_FAILURE;
}

bool GstEnginePipeline::Seek(qint64 nanosec) {
  return gst_element_seek_simple(pipeline_, GST_FORMAT_TIME,
                                 GST_SEEK_FLAG_FLUSH, nanosec);
}

void GstEnginePipeline::SetEqualizerEnabled(bool enabled) {
  g_object_set(G_OBJECT(equalizer_), "active", enabled, NULL);
}


void GstEnginePipeline::SetEqualizerParams(int preamp, const QList<int>& band_gains) {
  // Preamp
  g_object_set(G_OBJECT(equalizer_), "preamp", ( preamp + 100 ) / 2, NULL);

  // Gains
  std::vector<int> gains_temp;
  gains_temp.resize( band_gains.count() );
  for ( int i = 0; i < band_gains.count(); i++ )
    gains_temp[i] = band_gains.at( i ) + 100;

  g_object_set(G_OBJECT(equalizer_), "gain", &gains_temp, NULL);
}

void GstEnginePipeline::SetVolume(int percent) {
  volume_percent_ = percent;
  UpdateVolume();
}

void GstEnginePipeline::SetVolumeModifier(qreal mod) {
  volume_modifier_ = mod;
  UpdateVolume();
}

void GstEnginePipeline::UpdateVolume() {
  float vol = double(volume_percent_) * 0.01 * volume_modifier_;
  g_object_set(G_OBJECT(volume_), "volume", vol, NULL);
}

void GstEnginePipeline::StartFader(int duration_msec,
                                   QTimeLine::Direction direction,
                                   QTimeLine::CurveShape shape) {
  // If there's already another fader running then start from the same time
  // that one was already at.
  int start_time = direction == QTimeLine::Forward ? 0 : duration_msec;
  if (fader_ && fader_->state() == QTimeLine::Running)
    start_time = fader_->currentTime();

  fader_.reset(new QTimeLine(duration_msec, this));
  connect(fader_.get(), SIGNAL(valueChanged(qreal)), SLOT(SetVolumeModifier(qreal)));
  connect(fader_.get(), SIGNAL(finished()), SLOT(FaderTimelineFinished()));
  fader_->setDirection(direction);
  fader_->setCurveShape(shape);
  fader_->setCurrentTime(start_time);
  fader_->resume();

  fader_fudge_timer_.stop();

  SetVolumeModifier(fader_->currentValue());
}

void GstEnginePipeline::FaderTimelineFinished() {
  fader_.reset();

  // Wait a little while longer before emitting the finished signal (and
  // probably distroying the pipeline) to account for delays in the audio
  // server/driver.
  fader_fudge_timer_.start(kFaderFudgeMsec, this);
}

void GstEnginePipeline::timerEvent(QTimerEvent* e) {
  if (e->timerId() == fader_fudge_timer_.timerId()) {
    fader_fudge_timer_.stop();
    emit FaderFinished();
    return;
  }

  QObject::timerEvent(e);
}
