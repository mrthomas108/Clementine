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

#include "echonesttags.h"
#include "tagwidget.h"

#include <echonest/Artist.h>

#include <boost/scoped_ptr.hpp>

struct EchoNestTags::Request {
  Request(int id) : id_(id), artist_(new Echonest::Artist) {}

  int id_;
  boost::scoped_ptr<Echonest::Artist> artist_;
};

void EchoNestTags::FetchInfo(int id, const Song& metadata) {
  boost::shared_ptr<Request> request(new Request(id));
  request->artist_->setName(metadata.artist());

  QNetworkReply* reply = request->artist_->fetchTerms();
  connect(reply, SIGNAL(finished()), SLOT(RequestFinished()));
  requests_[reply] = request;
}

void EchoNestTags::RequestFinished() {
  QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
  if (!reply || !requests_.contains(reply))
    return;
  reply->deleteLater();

  RequestPtr request = requests_.take(reply);

  try {
    request->artist_->parseProfile(reply);
  } catch (Echonest::ParseError e) {
    qWarning() << "Error parsing echonest reply:" << e.errorType() << e.what();
  }

  if (!request->artist_->terms().isEmpty()) {
    CollapsibleInfoPane::Data data;
    data.id_ = "echonest/artisttags";
    data.title_ = tr("Artist tags");
    data.type_ = CollapsibleInfoPane::Data::Type_Tags;
    data.icon_ = QIcon(":/last.fm/icon_tag.png");

    TagWidget* widget = new TagWidget;
    data.contents_ = widget;

    widget->SetIcon(data.icon_);
    widget->SetUrlPattern("lastfm://globaltags/%1");

    foreach (const Echonest::Term& term, request->artist_->terms()) {
      widget->AddTag(term.name());
      if (widget->count() >= 10)
        break;
    }

    emit InfoReady(request->id_, data);
  }

  emit Finished(request->id_);
}

