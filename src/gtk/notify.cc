// -*-c++-*-

// notify.h
//
//  Copyright 1999-2008 Daniel Burrows
//  Copyright 2008 Obey Arthur Liu
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 2 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; see the file COPYING.  If not, write to
//  the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
//  Boston, MA 02111-1307, USA.

#include "notify.h"
#include "aptitude.h"

#include <iostream>

namespace gui
{

  Notification::Notification(const Glib::RefPtr<Gtk::TextBuffer> buffer, std::vector<Gtk::Button *> buttons)
  {
    Gtk::TextView * textview = manage(new Gtk::TextView(buffer));
    textview->show();
    pack_start(*textview, true, true);
    for (std::vector<Gtk::Button *>::iterator button_iter = buttons.begin();
    button_iter != buttons.end(); button_iter++)
    {
      (*button_iter)->show();
      pack_start(*manage(*button_iter), false, true);
    }
    Gtk::Button * close_button = manage(new Gtk::Button());
    Gtk::Image * close_button_image = manage(new Gtk::Image(Gtk::Stock::CLOSE, Gtk::ICON_SIZE_MENU));
    close_button->property_image() = close_button_image;
    close_button->signal_clicked().connect(close_clicked);
    close_button->show();
    pack_start(*close_button, false, true);
  }

  NotifyView::NotifyView(BaseObjectType* cobject, const Glib::RefPtr<Gnome::Glade::Xml>& refGlade) : Gtk::VBox(cobject)
  {
    refGlade->get_widget("main_notify_rows", rows);
  }

  NotifyView::~NotifyView()
  {
    // TODO Auto-generated destructor stub
  }

  void NotifyView::add_notification(Notification * notification)
  {
    notification->close_clicked.connect(sigc::bind(sigc::mem_fun(*this, &NotifyView::remove_notification), notification));
    notification->show();
    rows->pack_start(*notification);
  }

  void NotifyView::remove_notification(Notification * notification)
  {
    remove(*notification);
  }

}
