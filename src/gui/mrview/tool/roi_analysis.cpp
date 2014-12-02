/*
   Copyright 2009 Brain Research Institute, Melbourne, Australia

   Written by J-Donald Tournier, 13/11/09.

   This file is part of MRtrix.

   MRtrix is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   MRtrix is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with MRtrix.  If not, see <http://www.gnu.org/licenses/>.

*/

//#define GL_DEBUG

#include "mrtrix.h"
#include "gui/mrview/window.h"
#include "gui/mrview/tool/roi_analysis.h"
#include "gui/mrview/volume.h"
#include "gui/dialog/file.h"
#include "gui/mrview/tool/list_model_base.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {
      namespace Tool
      {

            
       namespace {
         constexpr std::array<std::array<GLubyte,3>,6> preset_colours = {
           255, 255, 0,
           255, 0, 255,
           0, 255, 255,
           255, 0, 0,
           0, 255, 255,
           0, 0, 255
         };
       }


        class ROI::Item : public Volume {
          public:
            template <class InfoType>
              Item (const InfoType& info) : Volume (info) {
                type = gl::UNSIGNED_BYTE;
                format = gl::RED;
                internal_format = gl::R8;
                set_allowed_features (false, true, false);
                set_interpolate (false);
                set_use_transparency (true);
                set_min_max (0.0, 1.0);
                set_windowing (-1.0f, 0.0f);
                alpha = 1.0f;
                colour = preset_colours[current_preset_colour++];
                if (current_preset_colour >= 6)
                  current_preset_colour = 0;
                transparent_intensity = 0.4;
                opaque_intensity = 0.6;
                colourmap = ColourMap::index ("Colour");

                bind();
                allocate();
              }

            void load (const MR::Image::Header& header) {
              bind();
              MR::Image::Buffer<bool> buffer (header);
              auto vox = buffer.voxel();
              std::vector<GLubyte> data (vox.dim(0)*vox.dim(1));
              ProgressBar progress ("loading ROI image \"" + header.name() + "\"...");
              for (auto outer = MR::Image::Loop(2,3) (vox); outer; ++outer) {
                auto p = data.begin();
                for (auto inner = MR::Image::Loop (0,2) (vox); inner; ++inner) 
                  *(p++) = vox.value();
                upload_data ({ 0, 0, vox[2] }, { vox.dim(0), vox.dim(1), 1 }, reinterpret_cast<void*> (&data[0]));
                ++progress;
              }
            }






            struct UndoEntry {

              template <class InfoType>
                UndoEntry (InfoType& roi, int current_axis, int current_slice) 
              {
                from = {0, 0, 0}; 
                from[current_axis] = current_slice;
                size = { roi.info().dim(0), roi.info().dim(1), roi.info().dim(2) };
                size[current_axis] = 1;

                if (current_axis == 0) { slice_axes[0] = 1; slice_axes[1] = 2; }
                else if (current_axis == 1) { slice_axes[0] = 0; slice_axes[1] = 2; }
                else { slice_axes[0] = 0; slice_axes[1] = 1; }
                tex_size = { roi.info().dim(slice_axes[0]), roi.info().dim(slice_axes[1]) };

                if (!copy_program) {
                  GL::Shader::Vertex vertex_shader (
                      "layout(location = 0) in ivec3 vertpos;\n"
                      "void main() {\n"
                      "  gl_Position = vec4 (vertpos,1);\n"
                      "}\n");
                  GL::Shader::Fragment fragment_shader (
                      "uniform isampler3D tex;\n"
                      "uniform ivec3 position;\n"
                      "uniform ivec2 axes;\n"
                      "layout (location = 0) out vec3 color0;\n"
                      "void main() {\n"
                      "  ivec3 pos = position;\n"
                      "  pos[axes.x] = int(gl_FragCoord.x);\n"
                      "  pos[axes.y] = int(gl_FragCoord.y);\n"
                      "  color0.r = texelFetch (tex, pos, 0);\n"
                      "}\n");

                  copy_program.attach (vertex_shader);
                  copy_program.attach (fragment_shader);
                  copy_program.link();
                }

                if (!copy_vertex_array_object) {
                  copy_vertex_buffer.gen();
                  copy_vertex_array_object.gen();

                  copy_vertex_buffer.bind (gl::ARRAY_BUFFER);
                  copy_vertex_array_object.bind();

                  gl::EnableVertexAttribArray (0);
                  gl::VertexAttribIPointer (0, 3, gl::INT, 3*sizeof(GLint), (void*)0);

                  GLint vertices[12] = { 
                    -1, -1, 0,
                    -1, 1, 0,
                    1, 1, 0,
                    1, -1, 0,
                  };
                  gl::BufferData (gl::ARRAY_BUFFER, sizeof(vertices), vertices, gl::STREAM_DRAW);
                }
                else copy_vertex_array_object.bind();

                from = {0, 0, 0}; 
                from[current_axis] = current_slice;

                if (current_axis == 0) { slice_axes[0] = 1; slice_axes[1] = 2; }
                else if (current_axis == 1) { slice_axes[0] = 0; slice_axes[1] = 2; }
                else { slice_axes[0] = 0; slice_axes[1] = 1; }
                tex_size = { roi.info().dim(slice_axes[0]), roi.info().dim(slice_axes[1]) };


                // set up 2D texture to store slice:
                GL::Texture tex;
                tex.gen (gl::TEXTURE_2D);
                gl::PixelStorei (gl::UNPACK_ALIGNMENT, 1);
                gl::TexImage2D (gl::TEXTURE_2D, 0, gl::R8, tex_size[0], tex_size[1], 0, gl::RED, gl::UNSIGNED_BYTE, nullptr);

                // set up off-screen framebuffer to map textures onto:
                GL::FrameBuffer framebuffer;
                framebuffer.gen();
                tex.set_interp_on (false);
                framebuffer.attach_color (tex, 0);
                framebuffer.draw_buffers (0);
                framebuffer.check();

                // render slice onto framebuffer:
                gl::Disable (gl::DEPTH_TEST);
                gl::Disable (gl::BLEND);
                gl::Viewport (0, 0, tex_size[0], tex_size[1]);
                roi.texture().bind();
                copy_program.start();
                gl::Uniform3iv (gl::GetUniformLocation (copy_program, "position"), 1, from.data());
                gl::Uniform2iv (gl::GetUniformLocation (copy_program, "axes"), 1, slice_axes.data());
                gl::DrawArrays (gl::TRIANGLE_FAN, 0, 4);
                copy_program.stop();
                framebuffer.unbind();

                // retrieve texture contents to main memory:
                before.resize (tex_size[0]*tex_size[1]);
                gl::GetTexImage (gl::TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, (void*)(&before[0]));
                after = before;
              }

              template <class InfoType>
                void edit (InfoType& roi, const Point<>& pos, int current_mode, float radius) {
                  Point<> vox = roi.transform().scanner2voxel (pos);
                  int rad = int (std::ceil (radius));
                  radius *= radius;
                  std::array<int,3> a = { int(std::lround (vox[0])), int(std::lround (vox[1])), int(std::lround (vox[2])) };
                  std::array<int,3> b = { a[0]+1, a[1]+1, a[2]+1 };
                  a[slice_axes[0]] = std::max (0, a[slice_axes[0]]-rad);
                  a[slice_axes[1]] = std::max (0, a[slice_axes[1]]-rad);
                  b[slice_axes[0]] = std::min (roi.info().dim(slice_axes[0]), b[slice_axes[0]]+rad);
                  b[slice_axes[1]] = std::min (roi.info().dim(slice_axes[1]), b[slice_axes[1]]+rad);

                  for (int k = a[2]; k < b[2]; ++k)
                    for (int j = a[1]; j < b[1]; ++j)
                      for (int i = a[0]; i < b[0]; ++i)
                        if (Math::pow2(vox[0]-i) + Math::pow2 (vox[1]-j) + Math::pow2 (vox[2]-k) < radius)
                          after[i-from[0] + size[0] * (j-from[1] + size[1] * (k-from[2]))] = current_mode == 1 ? 1 : 0;

                  roi.texture().bind();
                  gl::TexSubImage3D (GL_TEXTURE_3D, 0, from[0], from[1], from[2], size[0], size[1], size[2], GL_RED, GL_UNSIGNED_BYTE, (void*) (&after[0]));
                }

              std::array<GLint,3> from, size;
              std::array<GLint,2> tex_size, slice_axes;
              std::vector<GLubyte> before, after;

              static GL::Shader::Program copy_program;
              static GL::VertexBuffer copy_vertex_buffer;
              static GL::VertexArrayObject copy_vertex_array_object;
            };
            std::vector<UndoEntry> undo_list;

            static int current_preset_colour;
        };

        GL::Shader::Program ROI::Item::UndoEntry::copy_program;
        GL::VertexBuffer ROI::Item::UndoEntry::copy_vertex_buffer;
        GL::VertexArrayObject ROI::Item::UndoEntry::copy_vertex_array_object;



        int ROI::Item::current_preset_colour = 0;



        class ROI::Model : public ListModelBase
        {
          public:
            Model (QObject* parent) : 
              ListModelBase (parent) { }

            void load (VecPtr<MR::Image::Header>& list);

            Item* get (QModelIndex& index) {
              return dynamic_cast<Item*>(items[index.row()]);
            }
        };


        void ROI::Model::load (VecPtr<MR::Image::Header>& list)
        {
          beginInsertRows (QModelIndex(), items.size(), items.size()+list.size());
          for (size_t i = 0; i < list.size(); ++i) {
            Item* roi = new Item (*list[i]);
            roi->load (*list[i]);
            items.push_back (roi);
          }
          endInsertRows();
        }


        ROI::ROI (Window& main_window, Dock* parent) :
          Base (main_window, parent),
          current_mode (0) { 
            VBoxLayout* main_box = new VBoxLayout (this);
            HBoxLayout* layout = new HBoxLayout;
            layout->setContentsMargins (0, 0, 0, 0);
            layout->setSpacing (0);

            QPushButton* button = new QPushButton (this);
            button->setToolTip (tr ("New ROI"));
            button->setIcon (QIcon (":/new.svg"));
            connect (button, SIGNAL (clicked()), this, SLOT (new_slot ()));
            layout->addWidget (button, 1);

            button = new QPushButton (this);
            button->setToolTip (tr ("Open ROI"));
            button->setIcon (QIcon (":/open.svg"));
            connect (button, SIGNAL (clicked()), this, SLOT (open_slot ()));
            layout->addWidget (button, 1);

            save_button = new QPushButton (this);
            save_button->setToolTip (tr ("Save ROI"));
            save_button->setIcon (QIcon (":/save.svg"));
            save_button->setEnabled (false);
            connect (save_button, SIGNAL (clicked()), this, SLOT (save_slot ()));
            layout->addWidget (save_button, 1);

            close_button = new QPushButton (this);
            close_button->setToolTip (tr ("Close ROI"));
            close_button->setIcon (QIcon (":/close.svg"));
            close_button->setEnabled (false);
            connect (close_button, SIGNAL (clicked()), this, SLOT (close_slot ()));
            layout->addWidget (close_button, 1);

            hide_all_button = new QPushButton (this);
            hide_all_button->setToolTip (tr ("Hide All"));
            hide_all_button->setIcon (QIcon (":/hide.svg"));
            hide_all_button->setCheckable (true);
            connect (hide_all_button, SIGNAL (clicked()), this, SLOT (hide_all_slot ()));
            layout->addWidget (hide_all_button, 1);

            main_box->addLayout (layout, 0);

            list_view = new QListView (this);
            list_view->setSelectionMode (QAbstractItemView::ExtendedSelection);
            list_view->setDragEnabled (true);
            list_view->viewport()->setAcceptDrops (true);
            list_view->setDropIndicatorShown (true);

            list_model = new Model (this);
            list_view->setModel (list_model);

            main_box->addWidget (list_view, 1);

            layout = new HBoxLayout;
            layout->setContentsMargins (0, 0, 0, 0);
            layout->setSpacing (0);

            draw_button = new QToolButton (this);
            draw_button->setToolButtonStyle (Qt::ToolButtonTextBesideIcon);
            QAction* action = new QAction (QIcon (":/draw.svg"), tr ("Draw"), this);
            action->setShortcut (tr ("D"));
            action->setToolTip (tr ("Add voxels to ROI"));
            action->setCheckable (true);
            action->setEnabled (false);
            connect (action, SIGNAL (triggered()), this, SLOT (draw_slot ()));
            draw_button->setDefaultAction (action);
            layout->addWidget (draw_button, 1);

            erase_button = new QToolButton (this);
            erase_button->setToolButtonStyle (Qt::ToolButtonTextBesideIcon);
            action = new QAction (QIcon (":/erase.svg"), tr ("Erase"), this);
            action->setShortcut (tr ("E"));
            action->setToolTip (tr ("Remove voxels from ROI"));
            action->setCheckable (true);
            action->setEnabled (false);
            connect (action, SIGNAL (triggered()), this, SLOT (erase_slot ()));
            erase_button->setDefaultAction (action);
            layout->addWidget (erase_button, 1);

            main_box->addLayout (layout, 0);

            colour_button = new QColorButton;
            colour_button->setEnabled (false);
            main_box->addWidget (colour_button, 0);
            connect (colour_button, SIGNAL (clicked()), this, SLOT (colour_changed()));


            opacity_slider = new QSlider (Qt::Horizontal);
            opacity_slider->setRange (1,1000);
            opacity_slider->setSliderPosition (int (1000));
            connect (opacity_slider, SIGNAL (valueChanged (int)), this, SLOT (opacity_changed(int)));
            opacity_slider->setEnabled (false);
            main_box->addWidget (new QLabel ("opacity"), 0);
            main_box->addWidget (opacity_slider, 0);

            connect (list_view->selectionModel(),
                SIGNAL(selectionChanged(const QItemSelection &, const QItemSelection &)),
                SLOT (selection_changed_slot(const QItemSelection &, const QItemSelection &)) );

            connect (list_model, SIGNAL (dataChanged (const QModelIndex&, const QModelIndex&)),
                     this, SLOT (toggle_shown_slot (const QModelIndex&, const QModelIndex&)));

            update_selection();
          }


        void ROI::new_slot ()
        {
          TRACE;
          //load (list);
        }



        void ROI::open_slot ()
        {
          std::vector<std::string> names = Dialog::File::get_images (this, "Select ROI images to open");
          if (names.empty())
            return;
          VecPtr<MR::Image::Header> list;
          for (size_t n = 0; n < names.size(); ++n)
            list.push_back (new MR::Image::Header (names[n]));

          load (list);
        }




        void ROI::save_slot ()
        {
          TRACE;
        }



        void ROI::load (VecPtr<MR::Image::Header>& list) 
        {
          size_t previous_size = list_model->rowCount();
          list_model->load (list);

          QModelIndex first = list_model->index (previous_size, 0, QModelIndex());
          QModelIndex last = list_model->index (list_model->rowCount()-1, 0, QModelIndex());
          list_view->selectionModel()->select (QItemSelection (first, last), QItemSelectionModel::Select);

          updateGL ();
        }



        void ROI::close_slot ()
        {
          QModelIndexList indexes = list_view->selectionModel()->selectedIndexes();
          while (indexes.size()) {
            list_model->remove_item (indexes.first());
            indexes = list_view->selectionModel()->selectedIndexes();
          }
          updateGL();
        }


        void ROI::draw_slot () 
        {
          if (erase_button->isChecked()) erase_button->setChecked (false);
          if (draw_button->isChecked()) grab_focus ();
          else release_focus ();
        }

        void ROI::erase_slot () 
        {
          if (draw_button->isChecked()) draw_button->setChecked (false);
          if (erase_button->isChecked()) grab_focus ();
          else release_focus ();
        }

        void ROI::hide_all_slot () 
        {
          updateGL();
        }


        void ROI::draw (const Projection& projection, bool is_3D, int, int)
        {
          if (is_3D) return;

          if (!is_3D) {
            // set up OpenGL environment:
            gl::Enable (gl::BLEND);
            gl::Disable (gl::DEPTH_TEST);
            gl::DepthMask (gl::FALSE_);
            gl::ColorMask (gl::TRUE_, gl::TRUE_, gl::TRUE_, gl::TRUE_);
            gl::BlendFunc (gl::SRC_ALPHA, gl::ONE_MINUS_SRC_ALPHA);
            gl::BlendEquation (gl::FUNC_ADD);
          }

          for (int i = 0; i < list_model->rowCount(); ++i) {
            if (list_model->items[i]->show && !hide_all_button->isChecked()) {
              Item* roi = dynamic_cast<Item*>(list_model->items[i]);
              //if (is_3D) 
                //window.get_current_mode()->overlays_for_3D.push_back (image);
              //else
                roi->render (shader, projection, projection.depth_of (window.focus()));
            }
          }

          if (!is_3D) {
            // restore OpenGL environment:
            gl::Disable (gl::BLEND);
            gl::Enable (gl::DEPTH_TEST);
            gl::DepthMask (gl::TRUE_);
          }
        }





        void ROI::toggle_shown_slot (const QModelIndex& index, const QModelIndex& index2) {
          if (index.row() == index2.row()) {
            list_view->setCurrentIndex(index);
          } else {
            for (size_t i = 0; i < list_model->items.size(); ++i) {
              if (list_model->items[i]->show) {
                list_view->setCurrentIndex (list_model->index (i, 0));
                break;
              }
            }
          }
          updateGL();
        }


        void ROI::update_slot (int) {
          updateGL();
        }

        void ROI::colour_changed () 
        {
          QModelIndexList indices = list_view->selectionModel()->selectedIndexes();
          for (int i = 0; i < indices.size(); ++i) {
            Item* roi = dynamic_cast<Item*> (list_model->get (indices[i]));
            QColor c = colour_button->color();
            roi->colour = { GLubyte (c.red()), GLubyte (c.green()), GLubyte (c.blue()) };
          }
          updateGL();
        }




        void ROI::opacity_changed (int)
        {
          QModelIndexList indices = list_view->selectionModel()->selectedIndexes();
          for (int i = 0; i < indices.size(); ++i) {
            Item* roi = dynamic_cast<Item*> (list_model->get (indices[i]));
            roi->alpha = opacity_slider->value() / 1.0e3f;
          }
          window.updateGL();
        }


        void ROI::selection_changed_slot (const QItemSelection &, const QItemSelection &)
        {
          update_selection();
        }



        void ROI::update_selection () 
        {
          QModelIndexList indices = list_view->selectionModel()->selectedIndexes();
          opacity_slider->setEnabled (indices.size());
          save_button->setEnabled (indices.size() == 1);
          close_button->setEnabled (indices.size());
          draw_button->defaultAction()->setEnabled (indices.size() == 1);
          erase_button->defaultAction()->setEnabled (indices.size() == 1);
          colour_button->setEnabled (indices.size());

          if (!indices.size()) {
            draw_button->setChecked (false);
            erase_button->setChecked (false);
            release_focus();
            return;
          }

          float opacity = 0.0f;
          float color[3] = { 0.0f, 0.0f, 0.0f };

          for (int i = 0; i < indices.size(); ++i) {
            Item* roi = dynamic_cast<Item*> (list_model->get (indices[i]));
            opacity += roi->alpha;
            color[0] += roi->colour[0];
            color[1] += roi->colour[1];
            color[2] += roi->colour[2];
          }
          opacity /= indices.size();
          colour_button->setColor (QColor (
                std::round (color[0] / indices.size()),
                std::round (color[1] / indices.size()),
                std::round (color[2] / indices.size()) ));

          opacity_slider->setValue (1.0e3f * opacity);
        }









        bool ROI::mouse_press_event () 
        { 
          if (draw_button->isChecked()) current_mode = 1;
          else if (erase_button->isChecked()) current_mode = 2;
          else current_mode = 0;

          QModelIndexList indices = list_view->selectionModel()->selectedIndexes();
          if (indices.size() != 1) {
            WARN ("FIXME: shouldn't be here!");
            return false;
          }


          const Projection* proj = window.get_current_mode()->get_current_projection();
          if (!proj) 
            return false;
          Point<> pos =  proj->screen_to_model (window.mouse_position(), window.focus());
          Point<> normal = proj->screen_normal();

          Item* roi = dynamic_cast<Item*> (list_model->get (indices[0]));
          // figure out the closest ROI axis, and lock to it:
          float x_dot_n = std::abs (roi->transform().image2scanner_dir (Point<> (1.0, 0.0, 0.0)).dot (normal));
          float y_dot_n = std::abs (roi->transform().image2scanner_dir (Point<> (0.0, 1.0, 0.0)).dot (normal));
          float z_dot_n = std::abs (roi->transform().image2scanner_dir (Point<> (0.0, 0.0, 1.0)).dot (normal));

          if (x_dot_n > y_dot_n) 
            current_axis = x_dot_n > z_dot_n ? 0 : 2;
          else 
            current_axis = y_dot_n > z_dot_n ? 1 : 2;

          // figure out current slice in ROI:
          current_slice = std::lround (roi->transform().scanner2voxel (pos)[current_axis]);

          // floating-point version of slice location to keep it consistent on
          // mouse move:
          Point<> slice_axis (0.0, 0.0, 0.0);
          slice_axis[current_axis] = current_axis == 2 ? 1.0 : -1.0;
          slice_axis = roi->transform().image2scanner_dir (slice_axis);
          current_slice_loc = pos.dot (slice_axis);


          // keep undo list bounded:
          size_t undo_list_max_size = 8;
          if (roi->undo_list.size() >= undo_list_max_size-1)
            roi->undo_list.erase (roi->undo_list.begin());
          
          // add new entry to undo list:
          roi->undo_list.push_back (Item::UndoEntry (*roi, current_axis, current_slice));

          // grab slice data from 3D texture:
          for (GLint j = 0; j < roi->undo_list.back().tex_size[1]; ++j) {
            for (GLint i = 0; i < roi->undo_list.back().tex_size[0]; ++i)
              std::cout << int(roi->undo_list.back().before[i+roi->undo_list.back().tex_size[0]*j]) << " ";
            std::cout << "\n";
          }

          roi->undo_list.back().edit (*roi, pos, current_mode, 5.0);

          updateGL();

          return true; 
        }


        bool ROI::mouse_move_event () 
        { 
          if (!current_mode) 
            return false;


          QModelIndexList indices = list_view->selectionModel()->selectedIndexes();
          if (!indices.size()) {
            WARN ("FIXME: shouldn't be here!");
            return false;
          }
          Item* roi = dynamic_cast<Item*> (list_model->get (indices[0]));

          const Projection* proj = window.get_current_mode()->get_current_projection();
          if (!proj) 
            return false;
          Point<> pos =  proj->screen_to_model (window.mouse_position(), window.focus());
          Point<> slice_axis (0.0, 0.0, 0.0);
          slice_axis[current_axis] = current_axis == 2 ? 1.0 : -1.0;
          slice_axis = roi->transform().image2scanner_dir (slice_axis);
          float l = (current_slice_loc - pos.dot (slice_axis)) / proj->screen_normal().dot (slice_axis);
          window.set_focus (window.focus() + l * proj->screen_normal());

          roi->undo_list.back().edit (*roi, pos + l * proj->screen_normal(), current_mode, 5.0);

          updateGL();

          return true; 
        }

        bool ROI::mouse_release_event () 
        { 
          current_mode = 0; 
          return true; 
        }





        bool ROI::process_batch_command (const std::string& cmd, const std::string& args)
        {
          (void)cmd;
          (void)args;
          /*

          // BATCH_COMMAND roi.load path # Loads the specified image on the roi tool.
          if (cmd == "roi.load") {
            VecPtr<MR::Image::Header> list;
            try { list.push_back (new MR::Image::Header (args)); }
            catch (Exception& e) { e.display(); }
            load (list);
            return true;
          }

          // BATCH_COMMAND roi.opacity value # Sets the roi opacity to floating value [0-1].
          else if (cmd == "roi.opacity") {
            try {
              float n = to<float> (args);
              opacity_slider->setSliderPosition(int(1.e3f*n));
            }
            catch (Exception& e) { e.display(); }
            return true;
          }

          */
          return false;
        }



      }
    }
  }
}




