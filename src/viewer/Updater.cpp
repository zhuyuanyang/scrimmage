/*!
 * @file
 *
 * @section LICENSE
 *
 * Copyright (C) 2017 by the Georgia Tech Research Institute (GTRI)
 *
 * This file is part of SCRIMMAGE.
 *
 *   SCRIMMAGE is free software: you can redistribute it and/or modify it under
 *   the terms of the GNU Lesser General Public License as published by the
 *   Free Software Foundation, either version 3 of the License, or (at your
 *   option) any later version.
 *
 *   SCRIMMAGE is distributed in the hope that it will be useful, but WITHOUT
 *   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *   FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 *   License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public License
 *   along with SCRIMMAGE.  If not, see <http://www.gnu.org/licenses/>.
 *
 * @author Kevin DeMarco <kevin.demarco@gtri.gatech.edu>
 * @author Eric Squires <eric.squires@gtri.gatech.edu>
 * @date 31 July 2017
 * @version 0.1.0
 * @brief Brief file description.
 * @section DESCRIPTION
 * A Long description goes here.
 *
 */

#include <iostream>

#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkDataSetMapper.h>
#include <vtkPNGReader.h>
#include <vtkOBJReader.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>
#include <vtkTriangle.h>
#include <vtkArrowSource.h>
#include <vtkVectorText.h>
#include <vtkCamera.h>
#include <vtkJPEGReader.h>
#include <vtkPolyDataReader.h>
#include <vtkSmoothPolyDataFilter.h>
#include <vtkPolyDataNormals.h>
#include <vtkTextureMapToPlane.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolygon.h>
#include <vtkTextActor.h>
#include <vtkTextProperty.h>
#include <vtkCallbackCommand.h>
#include <vtkCellArray.h>
#include <vtkSphereSource.h>
#include <vtkPyramid.h>
#include <vtkUnstructuredGrid.h>
#include <vtkConeSource.h>
#include <vtkVertexGlyphFilter.h>
#include <vtkPlaneSource.h>
#include <vtkRegularPolygonSource.h>

#include <scrimmage/viewer/Updater.h>
#include <scrimmage/math/Quaternion.h>
#include <scrimmage/math/Angles.h>
#include <scrimmage/common/Utilities.h>
#include <scrimmage/proto/ProtoConversions.h>
#include <scrimmage/parse/ConfigParse.h>
#include <scrimmage/parse/ParseUtils.h>

using std::cout;
using std::endl;

#define BILLION 1000000000L

namespace sp = scrimmage_proto;
namespace sc = scrimmage;

namespace scrimmage {

    double fps = 0;
    void fpsCallbackFunction(vtkObject* caller, long unsigned int vtkNotUsed(eventId),
                             void* vtkNotUsed(clientData), void* vtkNotUsed(callData))
    {
        vtkRenderer* renderer = static_cast<vtkRenderer*>(caller);
        double timeInSeconds = renderer->GetLastRenderTimeInSeconds();
        fps = 1.0/timeInSeconds;
    }

    Updater *Updater::New()
    {
        Updater *cb = new Updater;
        return cb;
    }

    Updater::Updater() : update_count(0), follow_offset_(50)
    {
        prev_time.tv_nsec = 0;
        prev_time.tv_sec = 0;
        max_update_rate_ = 1.0;

        reset_scale();
        scale_required_ = false;

        follow_id_ = 0;

        view_mode_ = ViewMode::FOLLOW;
        enable_trails_ = false;

        gui_msg_.set_inc_warp(false);
        gui_msg_.set_dec_warp(false);
        gui_msg_.set_toggle_pause(false);
        gui_msg_.set_single_step(false);

        send_shutdown_msg_ = true;
    }

    void Updater::init()
    {
        // Create a default grid:
        grid_ = std::make_shared<Grid>();
        grid_->create(10000, 100, renderer_);

        // Create a default origin:
        origin_axes_ = std::make_shared<OriginAxes>();
        origin_axes_->create(1, renderer_);

        create_text_display();

        enable_fps();
    }

    void Updater::Execute(vtkObject *caller, unsigned long eventId,
                          void * vtkNotUsed(callData))
    {
        update();

        vtkRenderWindowInteractor *iren = vtkRenderWindowInteractor::SafeDownCast(caller);
        iren->GetRenderWindow()->Render();
    }

    bool Updater::update()
    {
        // Make sure we don't update the contacts faster than the max rate
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t diff = BILLION * (now.tv_sec - prev_time.tv_sec) + now.tv_nsec - prev_time.tv_nsec;
        if (diff < (1.0/max_update_rate_*BILLION) ) {
            return true;
        }
        prev_time = now;

        ///////////////////////////////////////////////////////////////////////
        // Network / Shared Memory Messages
        ///////////////////////////////////////////////////////////////////////

        // Do we have any updates to the terrain info
        if (incoming_interface_->utm_terrain_update()) {
            incoming_interface_->utm_terrain_mutex.lock();
            auto &utms = incoming_interface_->utm_terrain();

            // We only care about the last message
            auto &utm = utms.back();

            // Process utm terrain data
            update_utm_terrain(utm);

            utms.clear();
            incoming_interface_->utm_terrain_mutex.unlock();
        }

        // Do we have any updates to the contact_visuals
        if (incoming_interface_->contact_visual_update()) {
            incoming_interface_->contact_visual_mutex.lock();
            auto &cv = incoming_interface_->contact_visual();
            auto it = cv.begin();
            while (it != cv.end()) {
                (*it)->set_update_required(true);
                contact_visuals_[(*it)->id()] = *it;
                cv.erase(it++);
            }
            incoming_interface_->contact_visual_mutex.unlock();
        }

        // Do we have any updates to the frames?
        if (incoming_interface_->frames_update()) {
            incoming_interface_->frames_mutex.lock();
            auto &frames = incoming_interface_->frames();

            // Check to see if we have to remove any contacts.
            // Need to check every frame so we don't miss a discrete removal
            // Maybe we make this it's own message one day.
            for (auto it : frames) {
                for (int i = 0; i < it->contact_size(); i++) {
                    if (!it->contact(i).active()) {
                        auto it_ac = actor_contacts_.find(it->contact(i).id().id());
                        if (it_ac != actor_contacts_.end()) {
                            it_ac->second->remove = true;
                        }
                    }
                }
            }

            // We only care about the last frame for display purposes
            auto &frame = frames.back();
            update_contacts(frame);
            frames.clear();

            // We want the shapes' ttl counter to be linked to newly received
            // frames. Update the shapes on a newly received frame.
            update_shapes();

            incoming_interface_->frames_mutex.unlock();
        }

        // Do we have any updates to the sim info?
        if (incoming_interface_->sim_info_update()) {
            incoming_interface_->sim_info_mutex.lock();
            auto &info_list = incoming_interface_->sim_info();

            // Check for shutting_down message from simcontrol
            for (auto it : info_list) {
                if (it.shutting_down()) {
                    send_shutdown_msg_ = false;
                    rwi_->GetRenderWindow()->Finalize();
                    rwi_->TerminateApp();
                }
            }

            // We only care about the last message for actual data
            sim_info_ = info_list.back();
            info_list.clear();
            incoming_interface_->sim_info_mutex.unlock();
        }

        // Do we have any new shapes?
        if (incoming_interface_->shapes_update()) {
            incoming_interface_->shapes_mutex.lock();
            auto &shapes_list = incoming_interface_->shapes();

            auto it = shapes_list.begin();
            while (it != shapes_list.end()) {
                draw_shapes(*it);

                // Update ttl for shapes that aren't the last in the list
                if (++it != shapes_list.end()) {
                    update_shapes();
                }
                --it;
                shapes_list.erase(it++);
            }
            incoming_interface_->shapes_mutex.unlock();
        }

        // Update scale
        if (scale_required_) {
            update_scale();
            scale_required_ = false;
        }

        ///////////////////////////////////////////////////////////////////////
        // Update camera and GUI elements
        ///////////////////////////////////////////////////////////////////////
        update_camera();
        update_text_display();

        return true;
    }

    bool Updater::update_scale()
    {
        for (auto &kv : actor_contacts_) {
            // Update actor scale:
            // Scale the actor based on the GUI control input:
            double scale_amount = 1.0;
            auto it = contact_visuals_.find(kv.first);
            if (it != contact_visuals_.end()) {
                scale_amount = it->second->scale() * scale_;
            }

            double scale_data[3];
            for (int i = 0; i < 3; i++) {
                scale_data[i] = scale_amount;
            }
            kv.second->actor->SetScale(scale_data[0], scale_data[1],
                                       scale_data[2]);
        }
        return true;
    }

    bool Updater::update_shapes()
    {
        // Remove past frames shapes that have reached time-to-live, if they
        // are not persistent
        auto it = shapes_.begin();
        while (it != shapes_.end()) {
            scrimmage_proto::Shape &s = std::get<0>(*it);
            s.set_ttl(s.ttl()-1);
            if (!s.persistent() && s.ttl() <= 0) {
                renderer_->RemoveActor(std::get<1>(*it));
                shapes_.erase(it++);
            } else {
                ++it;
            }
        }
        return true;
    }

    bool Updater::draw_shapes(scrimmage_proto::Shapes &shapes)
    {
        // Display new shapes
        for (int i = 0; i < shapes.shape_size(); i++) {

            // Create the mapper and actor that each shape will use
            vtkSmartPointer<vtkPolyDataMapper> mapper =
                vtkSmartPointer<vtkPolyDataMapper>::New();
            vtkSmartPointer<vtkActor> actor =
                vtkSmartPointer<vtkActor>::New();

            const scrimmage_proto::Shape shape = shapes.shape(i);
            bool status = false;
            if (shape.type() == scrimmage_proto::Shape::Triangle) {
                status = draw_triangle(shape, actor, mapper);
            } else if (shape.type() == scrimmage_proto::Shape::Arrow) {
                status = draw_arrow(shape, actor, mapper);
            } else if (shape.type() == scrimmage_proto::Shape::Cone) {
                status = draw_cone(shape, actor, mapper);
            } else if (shape.type() == scrimmage_proto::Shape::Line) {
                status = draw_line(shape, actor, mapper);
            } else if (shape.type() == scrimmage_proto::Shape::Polygon) {
                status = draw_polygon(shape, actor, mapper);
            } else if (shape.type() == scrimmage_proto::Shape::Polydata) {
                status = draw_polydata(shape, actor, mapper);
            } else if (shape.type() == scrimmage_proto::Shape::Plane) {
                status = draw_plane(shape, actor, mapper);
            } else if (shape.type() == scrimmage_proto::Shape::Pointcloud) {
                status = draw_pointcloud(shape, actor, mapper);
            } else if (shape.type() == scrimmage_proto::Shape::Circle) {
                status = draw_circle(shape, actor, mapper);
            } else if (shape.type() == scrimmage_proto::Shape::Sphere) {
                status = draw_sphere(shape, actor, mapper);
            } else if (shape.type() == scrimmage_proto::Shape::Text) {
                status = draw_text(shape, actor, mapper);
            } else {
                status = draw_sphere(shape, actor, mapper);
            }

            // Only add the actor if it was correctly constructed
            if (status) {
                double opacity = shape.opacity();
                if (opacity < 0.00001) {
                    actor->GetProperty()->SetOpacity(1.0);
                } else {
                    actor->GetProperty()->SetOpacity(shape.opacity());
                }
                actor->GetProperty()->SetColor(shape.color().r()/255.0,
                                               shape.color().g()/255.0,
                                               shape.color().b()/255.0);
                renderer_->AddActor(actor);

                shapes_.push_back(std::make_pair(shape, actor));

                // Since protobufs default value for int is 0, if the ttl is 0
                // at this point, set ttl to 1. We are creating new shapes
                // here, so it doesn't make sense for a shape to have a ttl
                // less than 1 here.
                if (std::get<0>(shapes_.back()).ttl() <= 0) {
                    std::get<0>(shapes_.back()).set_ttl(1);
                }
            }
        }
        return true;
    }

    bool Updater::update_camera()
    {
        // Free mode if contacts don't exist
        if (actor_contacts_.size() == 0) {
            view_mode_ = ViewMode::FREE;
            return true;
        }

        // Handle changing follow ids
        if (inc_follow_) {
            follow_id_++;
        } else if (dec_follow_) {
            follow_id_--;
        }

        // Find min/max ids
        int min = std::numeric_limits<int>::infinity();
        int max = -std::numeric_limits<int>::infinity();
        for (auto &kv : actor_contacts_) {
            int id = kv.second->contact.id().id();
            if (id < min) {
                min = id;
            } else if (id > max) {
                max = id;
            }
        }

        auto it = actor_contacts_.find(follow_id_);
        while (it == actor_contacts_.end()) {
            if (follow_id_ > max) {
                follow_id_ = actor_contacts_.begin()->second->contact.id().id();
            } else if (follow_id_ < min) {
                follow_id_ = actor_contacts_.rbegin()->second->contact.id().id();
            } else if (inc_follow_) {
                follow_id_++;
            } else if (dec_follow_) {
                follow_id_--;
            } else {
                // The ID might have been removed, increment to search for next
                // available
                follow_id_++;
            }
            it = actor_contacts_.find(follow_id_);
        }
        inc_follow_ = dec_follow_ = false;

        double x_pos = it->second->contact.state().position().x();
        double y_pos = it->second->contact.state().position().y();
        double z_pos = it->second->contact.state().position().z();

        double camera_pos[3];
        if (view_mode_ == ViewMode::OFFSET) {
            camera_pos[0] = x_pos + 00;
            camera_pos[1] = y_pos - 150;
            camera_pos[2] = z_pos + 15;
            renderer_->GetActiveCamera()->SetPosition(camera_pos);
            renderer_->GetActiveCamera()->SetFocalPoint(x_pos, y_pos, z_pos);
        } else if (view_mode_ == ViewMode::FOLLOW) {
            Eigen::Vector3d base_offset(-50, 0, 15);
            Eigen::Vector3d rel_cam_pos = base_offset.normalized() * follow_offset_;
            Eigen::Vector3d unit_vector = rel_cam_pos / rel_cam_pos.norm();

            scrimmage_proto::Quaternion sp_quat = it->second->contact.state().orientation();
            sc::Quaternion quat(sp_quat.w(), sp_quat.x(), sp_quat.y(), sp_quat.z());

            unit_vector = quat.rotate(unit_vector);
            Eigen::Vector3d pos = Eigen::Vector3d(x_pos, y_pos, z_pos) +
                unit_vector * rel_cam_pos.norm();

            camera_pos[0] = pos(0);
            camera_pos[1] = pos(1);
            camera_pos[2] = pos(2);
            renderer_->GetActiveCamera()->SetPosition(camera_pos);
            renderer_->GetActiveCamera()->SetFocalPoint(x_pos, y_pos, z_pos);
        }

        return true;
    }

    bool Updater::update_text_display()
    {
        // Update FPS
        std::string fps_str = "FPS: " + std::to_string(fps);
        fps_actor_->SetInput(fps_str.c_str());

        // Update the time (text) display
        std::string time_str = std::to_string(frame_time_) + " s";
        time_actor_->SetInput(time_str.c_str());

        // Update the time warp
        std::string time_warp_str = std::to_string(sim_info_.desired_warp()) + " X";
        warp_actor_->SetInput(time_warp_str.c_str());

        // Display information about the aircraft we are following:
        auto it = actor_contacts_.find(follow_id_);
        if (it != actor_contacts_.end()) {
            scrimmage_proto::Quaternion sp_quat = it->second->contact.state().orientation();
            sc::Quaternion quat(sp_quat.w(), sp_quat.x(), sp_quat.y(), sp_quat.z());
            std::string heading_str = "H: " + std::to_string(sc::Angles::rad2deg(quat.yaw()));
            heading_actor_->SetInput(heading_str.c_str());

            std::string alt_str = "Alt: " + std::to_string(it->second->contact.state().position().z());
            alt_actor_->SetInput(alt_str.c_str());
        }

        return true;
    }


    void Updater::next_mode()
    {
        switch(view_mode_) {
        case ViewMode::FOLLOW:
            view_mode_ = ViewMode::FREE;
            break;
        case ViewMode::FREE:
            view_mode_ = ViewMode::OFFSET;
            break;
        case ViewMode::OFFSET:
            view_mode_ = ViewMode::FOLLOW;
            break;
        default:
            view_mode_ = ViewMode::FOLLOW;
            break;
        }
    }

    bool Updater::update_utm_terrain(std::shared_ptr<scrimmage_proto::UTMTerrain> &utm)
    {
        // Reset the grid
        grid_->remove();
        if (utm->enable_grid()) {
            grid_->create(utm->grid_size(), utm->grid_spacing(), renderer_);
        }

        // Reset / Show the origin?
        origin_axes_->remove();
        if (utm->show_origin()) {
            origin_axes_->create(utm->origin_length(), renderer_);
        }

        // Set the background color:
        renderer_->SetBackground(utm->background().r()/255.0,
                                 utm->background().g()/255.0,
                                 utm->background().b()/255.0);

        // Exit if the terrain is disabled in this messasge
        if (!utm->enable_terrain()) return true;

        // If the terrain data already exists in our map, use that instead:
        auto it = terrain_map_.find(utm->terrain_name());
        if (it != terrain_map_.end()) {
            utm = it->second;
        } else {
            // Search for the appropriate files on the local system
            ConfigParse terrain_parse;
            find_terrain_files(utm->terrain_name(),
                               terrain_parse, utm);
            terrain_map_[utm->terrain_name()] = utm;
        }

        if (utm->enable_terrain()) {
            // Read and create texture...
            vtkSmartPointer<vtkJPEGReader> terrain_jPEGReader =
                vtkSmartPointer<vtkJPEGReader>::New();
            terrain_jPEGReader->SetFileName(utm->texture_file().c_str());
            terrain_jPEGReader->Update();

            // Apply the texture
            vtkSmartPointer<vtkTexture> terrain_texture =
                vtkSmartPointer<vtkTexture>::New();
            terrain_texture->SetInputConnection(terrain_jPEGReader->GetOutputPort());
            terrain_texture->InterpolateOn();

            // Read the terrain polydata
            vtkSmartPointer<vtkPolyDataReader> terrain_reader1 =
                vtkSmartPointer<vtkPolyDataReader>::New();
            terrain_reader1->SetFileName(utm->poly_data_file().c_str());
            terrain_reader1->Update();

            vtkSmartPointer<vtkPolyData> polydata;
            polydata = terrain_reader1->GetOutput();

            // Setup colors
            vtkSmartPointer<vtkUnsignedCharArray> colors =
                vtkSmartPointer<vtkUnsignedCharArray>::New();
            colors->SetNumberOfComponents(3);
            colors->SetName ("Colors");
            for (int i = 0; i < polydata->GetNumberOfPoints(); ++i) {
                unsigned char tempColor[3] = {255,255,255};
#if VTK_MAJOR_VERSION <= 6
                colors->InsertNextTupleValue(tempColor);
#else
                colors->InsertNextTypedTuple(tempColor);
#endif
            }

            terrain_reader1->Update();
            polydata->GetPointData()->SetScalars(colors);

            vtkSmartPointer<vtkTransform> translation =
                vtkSmartPointer<vtkTransform>::New();
            translation->Translate(-utm->x_translate(), -utm->y_translate(),
                                   -utm->z_translate());

            vtkSmartPointer<vtkTransformPolyDataFilter> transformFilter =
                vtkSmartPointer<vtkTransformPolyDataFilter>::New();
            transformFilter->SetInputConnection(terrain_reader1->GetOutputPort());

            transformFilter->SetTransform(translation);
            transformFilter->Update();

            // Smooth poly data
            vtkSmartPointer<vtkSmoothPolyDataFilter> smoothFilter =
                vtkSmartPointer<vtkSmoothPolyDataFilter>::New();
            smoothFilter->SetInputConnection(transformFilter->GetOutputPort());
            smoothFilter->SetNumberOfIterations(15);
            smoothFilter->SetRelaxationFactor(0.1);
            smoothFilter->FeatureEdgeSmoothingOff();
            smoothFilter->BoundarySmoothingOn();
            smoothFilter->Update();

            // Update normals on newly smoothed polydata
            vtkSmartPointer<vtkPolyDataNormals> normalGenerator = vtkSmartPointer<vtkPolyDataNormals>::New();
            normalGenerator->SetInputConnection(smoothFilter->GetOutputPort());
            normalGenerator->ComputePointNormalsOn();
            normalGenerator->ComputeCellNormalsOn();
            normalGenerator->Update();

            vtkSmartPointer<vtkTextureMapToPlane> texturePlane =
                vtkSmartPointer<vtkTextureMapToPlane>::New();
            texturePlane->SetInputConnection(normalGenerator->GetOutputPort());

            vtkSmartPointer<vtkPolyDataMapper> terrain_mapper =
                vtkSmartPointer<vtkPolyDataMapper>::New();
            terrain_mapper->SetInputConnection(texturePlane->GetOutputPort());

            // Remove the old terrain actor if it exists already
            if (terrain_actor_ != NULL) {
                renderer_->RemoveActor(terrain_actor_);
            }
            terrain_actor_ = vtkSmartPointer<vtkActor>::New();
            terrain_actor_->SetMapper(terrain_mapper);
            terrain_actor_->SetTexture(terrain_texture);
            terrain_actor_->GetProperty()->SetColor(0,0,0);

            renderer_->AddActor(terrain_actor_);
        }
        return true;
    }

    void Updater::set_max_update_rate(double max_update_rate)
    { max_update_rate_ = max_update_rate; }

    void Updater::set_renderer(vtkSmartPointer<vtkRenderer> &renderer)
    { renderer_ = renderer; }

    void Updater::set_rwi(vtkSmartPointer<vtkRenderWindowInteractor> &rwi)
    { rwi_ = rwi; }

    void Updater::set_incoming_interface(InterfacePtr &incoming_interface)
    { incoming_interface_ = incoming_interface; }

    void Updater::set_outgoing_interface(InterfacePtr &outgoing_interface)
    { outgoing_interface_ = outgoing_interface; }

    bool Updater::update_contacts(std::shared_ptr<scrimmage_proto::Frame> &frame)
    {
        frame_time_ = frame->time();

        // Add new contacts to contact map
        for (int i = 0; i < frame->contact_size(); i++) {

            const scrimmage_proto::Contact cnt = frame->contact(i);
            int id = cnt.id().id();

            //cout << "------" << endl;
            //cout << "new contact: " << id << endl;
            //cout << "pos: " << cnt.state().position().x() << ", " << cnt.state().position().y() << ", " << cnt.state().position().z() << endl;

            if (actor_contacts_.count(id) == 0 && cnt.active()) {
                // Initialize everything as a sphere until it can be matched
                // with information in the contact_visuals_ map
                vtkSmartPointer<vtkSphereSource> sphereSource =
                    vtkSmartPointer<vtkSphereSource>::New();
                sphereSource->SetCenter(0,0,0);
                sphereSource->SetRadius(1);

                //Create a mapper
                vtkSmartPointer<vtkDataSetMapper> mapper =
                    vtkSmartPointer<vtkDataSetMapper>::New();
                mapper->SetInputConnection(sphereSource->GetOutputPort());

                // Create an actor
                vtkSmartPointer<vtkActor> actor =
                    vtkSmartPointer<vtkActor>::New();
                actor->SetMapper(mapper);

                actor->GetProperty()->SetColor(161,161,161);
                actor->GetProperty()->SetOpacity(0.25);

                renderer_->AddActor(actor);

                // Add the object label
                vtkSmartPointer<vtkVectorText> textSource =
                    vtkSmartPointer<vtkVectorText>::New();
                textSource->SetText( std::to_string(id).c_str() );

                // Create a mapper for the label
                vtkSmartPointer<vtkPolyDataMapper> label_mapper =
                    vtkSmartPointer<vtkPolyDataMapper>::New();
                label_mapper->SetInputConnection( textSource->GetOutputPort() );

                // Create a subclass of vtkActor: a vtkFollower that remains facing the camera
                vtkSmartPointer<vtkFollower> label =
                    vtkSmartPointer<vtkFollower>::New();
                label->SetMapper( label_mapper );
                label->GetProperty()->SetColor( 1, 1, 1 ); // white

                // Add the actor to the scene
                //renderer_->AddActor(actor);
                renderer_->AddActor(label);

                // Save a reference to the actor for modifying later
                std::shared_ptr<ActorContact> actor_contact = std::make_shared<ActorContact>();
                actor_contact->actor = actor;

                sc::set(actor_contact->color, 161, 161, 161);
                actor_contact->contact = cnt;
                actor_contact->label = label;
                actor_contact->exists = true;
                actor_contact->remove = false;
                actor_contacts_[id] = actor_contact;
            }
        }

        // Update contacts in contact map
        for (int i = 0; i < frame->contact_size(); i++) {
            const scrimmage_proto::Contact cnt = frame->contact(i);
            int id = cnt.id().id();

            if (actor_contacts_.count(id) > 0) {
                // Update an existing contact
                std::shared_ptr<ActorContact> ac = actor_contacts_[id];
                ac->exists = true;
                ac->contact = cnt;

                // Update the visuals for the actor, if neccessary
                auto it = contact_visuals_.find(id);
                if (it != contact_visuals_.end() && it->second->update_required()) {
                    update_contact_visual(ac, it->second);
                    it->second->set_update_required(false);
                }

                double x_pos = cnt.state().position().x();
                double y_pos = cnt.state().position().y();
                double z_pos = cnt.state().position().z();

                ac->actor->SetPosition(x_pos, y_pos, z_pos);

                scrimmage_proto::Quaternion sp_quat = cnt.state().orientation();
                sc::Quaternion quat(sp_quat.w(), sp_quat.x(), sp_quat.y(), sp_quat.z());
                Eigen::Matrix3d mat3 = quat.toRotationMatrix();

                // Set rotation matrix for actor
                double scale_data[3];
                ac->actor->GetScale(scale_data);

                vtkMatrix4x4 *m;
                m = ac->actor->GetMatrix();

                for (int r = 0; r < 3; r++) {
                    for (int c = 0; c < 3; c++) {
                        m->SetElement(r,c, scale_data[0]*mat3(r,c)); //TODO: scale data should
                    }
                }

                // Tells each label to "face" the camera
                ac->label->SetCamera( renderer_->GetActiveCamera() );
                ac->label->SetPosition(x_pos, y_pos, z_pos + 1.5);

                update_trail(ac, x_pos, y_pos, z_pos);
            }
        }

        // Set opacity of stale actors
        for (auto &kv : actor_contacts_) {
            if (!kv.second->exists) {
                kv.second->actor->GetProperty()->SetOpacity(0.10);
                kv.second->label->GetProperty()->SetOpacity(0.10);
            }
            kv.second->exists = false;
        }

        // Remove actors that had an inactive tag in their message
        auto it = actor_contacts_.begin();
        while (it != actor_contacts_.end()) {
            if (it->second->remove) {
                // Remove actor and label from viewer
                renderer_->RemoveActor(it->second->actor);
                renderer_->RemoveActor(it->second->label);

                // Remove the trail points
                for (std::list<vtkSmartPointer<vtkActor> >::iterator it_trail = it->second->trail.begin();
                     it_trail != it->second->trail.end(); it_trail++) {
                    renderer_->RemoveActor(*it_trail);
                }
                actor_contacts_.erase(it++); // remove map entry
            } else {
                it++;
            }
        }

        return true;
    }

    void Updater::update_contact_visual(std::shared_ptr<ActorContact> &actor_contact,
                                        std::shared_ptr<scrimmage_proto::ContactVisual> &cv)
    {
        // Only update the meshes if the model name has changed
        if (actor_contact->model_name != cv->name()) {
            actor_contact->model_name = cv->name();

            // Remove the old actor:
            renderer_->RemoveActor(actor_contact->actor);

            //Create a new actor and mapper
            vtkSmartPointer<vtkDataSetMapper> mapper =
                vtkSmartPointer<vtkDataSetMapper>::New();

            vtkSmartPointer<vtkActor> actor = vtkSmartPointer<vtkActor>::New();

            // Search for the appropriate files on the local system
            ConfigParse cv_parse;
            FileSearch file_search;
            bool mesh_found, texture_found;
            find_model_properties(cv->name(), cv_parse, file_search, cv,
                                  mesh_found, texture_found);

            if (actor_contact->contact.type() == scrimmage_proto::MESH) {
                if (texture_found) {
                    vtkSmartPointer<vtkPNGReader> pngReader =
                        vtkSmartPointer<vtkPNGReader>::New();
                    pngReader->SetFileName(cv->texture_file().c_str());
                    pngReader->Update();

                    vtkSmartPointer<vtkTexture> colorTexture =
                        vtkSmartPointer<vtkTexture>::New();
                    colorTexture->SetInputConnection(pngReader->GetOutputPort());
                    colorTexture->InterpolateOn();
                    actor->SetTexture(colorTexture);
                }

                vtkSmartPointer<vtkOBJReader> reader =
                    vtkSmartPointer<vtkOBJReader>::New();
                reader->SetFileName(cv->model_file().c_str());
                reader->Update();

                vtkSmartPointer<vtkTransform> transform = vtkSmartPointer<vtkTransform>::New();

                //transform->RotateWXYZ(cv->rotate(0), cv->rotate(1),
                //                      cv->rotate(2), cv->rotate(3));
                transform->RotateX(cv->rotate(0));
                transform->RotateY(cv->rotate(1));
                transform->RotateZ(cv->rotate(2));

                vtkSmartPointer<vtkTransformPolyDataFilter> transformFilter =
                    vtkSmartPointer<vtkTransformPolyDataFilter>::New();

                transformFilter->SetTransform(transform);
                transformFilter->SetInputConnection(reader->GetOutputPort());
                transformFilter->Update();

                mapper->SetInputConnection(transformFilter->GetOutputPort());

                // Need to scale contact to current GUI scale:
                double scale_data[3];
                for (int i = 0; i < 3; i++) {
                    scale_data[i] = cv->scale() * scale_;
                }
                actor->SetScale(scale_data[0], scale_data[1],
                                scale_data[2]);
            } else if (actor_contact->contact.type() == scrimmage_proto::AIRCRAFT) {
                vtkSmartPointer<vtkPoints> points =
                    vtkSmartPointer<vtkPoints>::New();

                float size = 2.0;
                float offset = -size/2;
                float p0[3] = {offset, 0, size/2};
                float p1[3] = {offset, 0, size/2};
                float p2[3] = {offset, -size, 0.0};
                float p3[3] = {offset, size, 0};
                float p4[3] = {size*2+offset, 0.0, 0.0};

                points->InsertNextPoint(p0);
                points->InsertNextPoint(p1);
                points->InsertNextPoint(p2);
                points->InsertNextPoint(p3);
                points->InsertNextPoint(p4);

                vtkSmartPointer<vtkPyramid> pyramid =
                    vtkSmartPointer<vtkPyramid>::New();
                pyramid->GetPointIds()->SetId(0,0);
                pyramid->GetPointIds()->SetId(1,1);
                pyramid->GetPointIds()->SetId(2,2);
                pyramid->GetPointIds()->SetId(3,3);
                pyramid->GetPointIds()->SetId(4,4);

                vtkSmartPointer<vtkCellArray> cells =
                    vtkSmartPointer<vtkCellArray>::New();
                cells->InsertNextCell (pyramid);

                vtkSmartPointer<vtkUnstructuredGrid> ug =
                    vtkSmartPointer<vtkUnstructuredGrid>::New();
                ug->SetPoints(points);
                ug->InsertNextCell(pyramid->GetCellType(),pyramid->GetPointIds());
                mapper->SetInputData(ug);

            } else if (actor_contact->contact.type() == scrimmage_proto::SPHERE) {
                vtkSmartPointer<vtkSphereSource> sphereSource =
                    vtkSmartPointer<vtkSphereSource>::New();
                sphereSource->SetCenter(0,0,0);
                sphereSource->SetRadius(1);

                mapper->SetInputConnection(sphereSource->GetOutputPort());

            } else {
                vtkSmartPointer<vtkSphereSource> sphereSource =
                    vtkSmartPointer<vtkSphereSource>::New();
                sphereSource->SetCenter(0,0,0);
                sphereSource->SetRadius(1);

                mapper->SetInputConnection(sphereSource->GetOutputPort());
            }

            actor->SetMapper(mapper);
            renderer_->AddActor(actor);
            actor_contact->actor = actor;
        }

        actor_contact->actor->GetProperty()->SetOpacity(cv->opacity());
        actor_contact->label->GetProperty()->SetOpacity(cv->opacity());

        if (cv->visual_mode() == scrimmage_proto::ContactVisual::COLOR) {
            actor_contact->actor->GetProperty()->SetColor(cv->color().r()/255.0,
                                                          cv->color().g()/255.0,
                                                          cv->color().b()/255.0);
        }

        sc::set(actor_contact->color, cv->color());
    }

    void Updater::update_trail(std::shared_ptr<ActorContact> &actor_contact,
                               double &x_pos, double &y_pos, double &z_pos)
    {
        if (enable_trails_) {
            /////////////////////
            // Create the geometry of a point (the coordinate)
            vtkSmartPointer<vtkPoints> points =
                vtkSmartPointer<vtkPoints>::New();
            const float p[3] = {(float)x_pos, (float)y_pos, (float)z_pos};

            // Create the topology of the point (a vertex)
            vtkSmartPointer<vtkCellArray> vertices =
                vtkSmartPointer<vtkCellArray>::New();
            vtkIdType pid[1];
            pid[0] = points->InsertNextPoint(p);
            vertices->InsertNextCell(1,pid);

            // Create a polydata object
            vtkSmartPointer<vtkPolyData> point =
                vtkSmartPointer<vtkPolyData>::New();

            // Set the points and vertices we created as the geometry and topology of the polydata
            point->SetPoints(points);
            point->SetVerts(vertices);

            // Visualize
            vtkSmartPointer<vtkPolyDataMapper> points_mapper =
                vtkSmartPointer<vtkPolyDataMapper>::New();

            points_mapper->SetInputData(point);

            vtkSmartPointer<vtkActor> points_actor =
                vtkSmartPointer<vtkActor>::New();
            points_actor->SetMapper(points_mapper);
            points_actor->GetProperty()->SetPointSize(5);

            points_actor->GetProperty()->SetColor(actor_contact->color.r()/255.0,
                                                  actor_contact->color.g()/255.0,
                                                  actor_contact->color.b()/255.0);

            renderer_->AddActor(points_actor);

            // Save the points actor, so that it can be removed
            // later.
            actor_contact->trail.push_back(points_actor);
            if (actor_contact->trail.size() > 20) {
                renderer_->RemoveActor(actor_contact->trail.front());
                actor_contact->trail.pop_front();
            }
        } else {
            // Remove the trails
            for (vtkSmartPointer<vtkActor> actor : actor_contact->trail) {
                renderer_->RemoveActor(actor);
            }
            actor_contact->trail.clear();
        }
    }

    void Updater::inc_follow()
    {
        inc_follow_ = true;
    }

    void Updater::dec_follow()
    {
        dec_follow_ = true;
    }

    void Updater::toggle_trails()
    {
        enable_trails_ = !enable_trails_;
    }

    void Updater::inc_warp()
    {
        gui_msg_.set_inc_warp(true);
        outgoing_interface_->send_gui_msg(gui_msg_);
        gui_msg_.set_inc_warp(false);
    }

    void Updater::dec_warp()
    {
        gui_msg_.set_dec_warp(true);
        outgoing_interface_->send_gui_msg(gui_msg_);
        gui_msg_.set_dec_warp(false);
    }

    void Updater::toggle_pause()
    {
        gui_msg_.set_toggle_pause(true);
        outgoing_interface_->send_gui_msg(gui_msg_);
        gui_msg_.set_toggle_pause(false);
    }

    void Updater::single_step()
    {
        gui_msg_.set_single_step(true);
        outgoing_interface_->send_gui_msg(gui_msg_);
        gui_msg_.set_single_step(false);
    }

    void Updater::request_cached()
    {
        gui_msg_.set_request_cached(true);
        outgoing_interface_->send_gui_msg(gui_msg_);
        gui_msg_.set_request_cached(false);
    }

    void Updater::shutting_down()
    {
        gui_msg_.set_shutting_down(true);
        if (send_shutdown_msg_) {
            outgoing_interface_->send_gui_msg(gui_msg_);
        }
    }

    void Updater::inc_scale()
    {
        scale_ *= 2.0;
        scale_required_ = true;
    }

    void Updater::dec_scale()
    {
        scale_ *= 0.5;
        scale_required_ = true;
    }

    void Updater::inc_follow_offset() {follow_offset_ *= 1.1;}

    void Updater::dec_follow_offset() {follow_offset_ /= 1.1;}

    void Updater::reset_scale()
    {
        scale_ = 1.0;
        scale_required_ = true;
    }

    void Updater::reset_view()
    {
        reset_scale();
        view_mode_ = ViewMode::FREE;
    }

    void Updater::create_text_display()
    {
        // Add the time (text) display
        time_actor_ = vtkSmartPointer<vtkTextActor>::New();
        time_actor_->SetInput("000.000 s");
        time_actor_->SetPosition(10, 10);
        time_actor_->GetTextProperty()->SetFontSize(24);
        time_actor_->GetTextProperty()->SetColor(1.0, 1.0, 1.0);
        renderer_->AddActor2D(time_actor_);

        // Add the warp (text) display
        warp_actor_ = vtkSmartPointer<vtkTextActor>::New();
        warp_actor_->SetInput("50.00 X");
        warp_actor_->SetPosition(300, 10);
        warp_actor_->GetTextProperty()->SetFontSize(24);
        warp_actor_->GetTextProperty()->SetColor(1.0, 1.0, 1.0);
        renderer_->AddActor2D(warp_actor_);

        // Add the heading (text) display
        heading_actor_ = vtkSmartPointer<vtkTextActor>::New();
        heading_actor_->SetInput("H: 360.00");
        heading_actor_->SetPosition(10, 100);
        heading_actor_->GetTextProperty()->SetFontSize(24);
        heading_actor_->GetTextProperty()->SetColor(1.0, 1.0, 1.0);
        renderer_->AddActor2D(heading_actor_);

        // Add the alt (text) display
        alt_actor_ = vtkSmartPointer<vtkTextActor>::New();
        alt_actor_->SetInput("Alt: 360.00");
        alt_actor_->SetPosition(10, 150);
        alt_actor_->GetTextProperty()->SetFontSize(24);
        alt_actor_->GetTextProperty()->SetColor(1.0, 1.0, 1.0);
        renderer_->AddActor2D(alt_actor_);

        // Add the fps (text) display
        fps_actor_ = vtkSmartPointer<vtkTextActor>::New();
        fps_actor_->SetInput("Fps: 60.0");
        fps_actor_->SetPosition(400, 10);
        fps_actor_->GetTextProperty()->SetFontSize(24);
        fps_actor_->GetTextProperty()->SetColor(1.0, 1.0, 1.0);
        renderer_->AddActor2D(fps_actor_);
    }

    void Updater::enable_fps()
    {
        // Set FPS callback:
        vtkSmartPointer<vtkCallbackCommand> callback =
            vtkSmartPointer<vtkCallbackCommand>::New();

        callback->SetCallback(fpsCallbackFunction);
        renderer_->AddObserver(vtkCommand::EndEvent, callback);
    }

    bool Updater::draw_triangle(const scrimmage_proto::Shape &s,
                                vtkSmartPointer<vtkActor> &actor,
                                vtkSmartPointer<vtkPolyDataMapper> &mapper)
    {
        vtkSmartPointer<vtkPoints> points =
            vtkSmartPointer<vtkPoints>::New();

        for (int i = 0; i < s.point_size(); i++) {
            points->InsertNextPoint(s.point(i).x(), s.point(i).y(),
                                    s.point(i).z());
        }

        vtkSmartPointer<vtkTriangle> triangle =
            vtkSmartPointer<vtkTriangle>::New();
        triangle->GetPointIds()->SetId ( 0, 0 );
        triangle->GetPointIds()->SetId ( 1, 1 );
        triangle->GetPointIds()->SetId ( 2, 2 );

        vtkSmartPointer<vtkCellArray> triangles =
            vtkSmartPointer<vtkCellArray>::New();
        triangles->InsertNextCell ( triangle );

        // Create a polydata object
        vtkSmartPointer<vtkPolyData> trianglePolyData =
            vtkSmartPointer<vtkPolyData>::New();

        // Add the geometry and topology to the polydata
        trianglePolyData->SetPoints ( points );
        trianglePolyData->SetPolys ( triangles );
        mapper->SetInputData(trianglePolyData);
        actor->SetMapper(mapper);

        return true;
    }

    bool Updater::draw_arrow(const scrimmage_proto::Shape &s,
                             vtkSmartPointer<vtkActor> &actor,
                             vtkSmartPointer<vtkPolyDataMapper> &mapper)
    {

        //Create an arrow.
        vtkSmartPointer<vtkArrowSource> arrowSource =
            vtkSmartPointer<vtkArrowSource>::New();

        double startPoint[3];
        double endPoint[3];

        if (s.point_size() != 2) {
            cout << "Invalid points size for drawing arrow." << endl;
            return false;
        }

        startPoint[0] = s.point(0).x();
        startPoint[1] = s.point(0).y();
        startPoint[2] = s.point(0).z();

        endPoint[0] = s.point(1).x();
        endPoint[1] = s.point(1).y();
        endPoint[2] = s.point(1).z();

        // Compute a basis
        double normalizedX[3];
        double normalizedY[3];
        double normalizedZ[3];

        // The X axis is a vector from start to end
        vtkMath::Subtract(endPoint, startPoint, normalizedX);
        double length = vtkMath::Norm(normalizedX);
        vtkMath::Normalize(normalizedX);

        // The Z axis is an arbitrary vector cross X
        double arbitrary[3];
        arbitrary[0] = 1;//vtkMath::Random(-10,10);
        arbitrary[1] = 2;//vtkMath::Random(-10,10);
        arbitrary[2] = 3;//vtkMath::Random(-10,10);
        vtkMath::Cross(normalizedX, arbitrary, normalizedZ);
        vtkMath::Normalize(normalizedZ);

        // The Y axis is Z cross X
        vtkMath::Cross(normalizedZ, normalizedX, normalizedY);
        vtkSmartPointer<vtkMatrix4x4> matrix =
            vtkSmartPointer<vtkMatrix4x4>::New();

        // Create the direction cosine matrix
        matrix->Identity();
        for (unsigned int i = 0; i < 3; i++)
        {
            matrix->SetElement(i, 0, normalizedX[i]);
            matrix->SetElement(i, 1, normalizedY[i]);
            matrix->SetElement(i, 2, normalizedZ[i]);
        }

        // Apply the transforms
        vtkSmartPointer<vtkTransform> transform =
            vtkSmartPointer<vtkTransform>::New();
        transform->Translate(startPoint);
        transform->Concatenate(matrix);
        transform->Scale(length, length, length);

        // Transform the polydata
        vtkSmartPointer<vtkTransformPolyDataFilter> transformPD =
            vtkSmartPointer<vtkTransformPolyDataFilter>::New();
        transformPD->SetTransform(transform);
        transformPD->SetInputConnection(arrowSource->GetOutputPort());

#ifdef USER_MATRIX
        mapper->SetInputConnection(arrowSource->GetOutputPort());
        actor->SetUserMatrix(transform->GetMatrix());
#else
        mapper->SetInputConnection(transformPD->GetOutputPort());
#endif
        actor->SetMapper(mapper);

        return true;
    }

    bool Updater::draw_cone(const scrimmage_proto::Shape &s,
                            vtkSmartPointer<vtkActor> &actor,
                            vtkSmartPointer<vtkPolyDataMapper> &mapper)
    {
        //Create a cone
        vtkSmartPointer<vtkConeSource> coneSource =
            vtkSmartPointer<vtkConeSource>::New();

        coneSource->SetRadius(s.base_radius());
        coneSource->SetHeight(s.height());
        coneSource->SetResolution(32);

        Eigen::Vector3d cone_dir = sc::eigen(s.direction());
        Eigen::Vector3d apex_shift = sc::eigen(s.apex()) + cone_dir.normalized()*s.height()/2.0;
        Eigen::Vector3d dir_shift = -cone_dir;
        coneSource->SetCenter(apex_shift(0), apex_shift(1), apex_shift(2));
        coneSource->SetDirection(dir_shift(0), dir_shift(1), dir_shift(2));
        coneSource->Update();

        mapper->SetInputConnection(coneSource->GetOutputPort());
        actor->SetMapper(mapper);
        return true;
    }

    bool Updater::draw_line(const scrimmage_proto::Shape &s,
                            vtkSmartPointer<vtkActor> &actor,
                            vtkSmartPointer<vtkPolyDataMapper> &mapper)
    {
        vtkSmartPointer<vtkLineSource> lineSource =
            vtkSmartPointer<vtkLineSource>::New();

        if (s.point_size() != 2) {
            cout << "Invalid points size for drawing line." << endl;
            return false;
        }

        lineSource->SetPoint1(s.point(0).x(), s.point(0).y(),
                              s.point(0).z());
        lineSource->SetPoint2(s.point(1).x(), s.point(1).y(),
                              s.point(1).z());
        lineSource->Update();

        mapper->SetInputConnection(lineSource->GetOutputPort());
        actor->SetMapper(mapper);
        actor->GetProperty()->SetLineWidth(2);
        return true;
    }

    bool Updater::draw_polygon(const scrimmage_proto::Shape &s,
                               vtkSmartPointer<vtkActor> &actor,
                               vtkSmartPointer<vtkPolyDataMapper> &mapper)
    {
        // Setup points
        vtkSmartPointer<vtkPoints> points =
            vtkSmartPointer<vtkPoints>::New();

        // Create the polygon
        vtkSmartPointer<vtkPolygon> polygon =
            vtkSmartPointer<vtkPolygon>::New();

        polygon->GetPointIds()->SetNumberOfIds(s.point_size());

        for (int i = 0; i < s.point_size(); i++) {
            points->InsertNextPoint(s.point(i).x(), s.point(i).y(),
                                    s.point(i).z());
            polygon->GetPointIds()->SetId(i, i);
        }

        // Add the polygon to a list of polygons
        vtkSmartPointer<vtkCellArray> polygons =
            vtkSmartPointer<vtkCellArray>::New();
        polygons->InsertNextCell(polygon);

        // Create a PolyData
        vtkSmartPointer<vtkPolyData> polygonPolyData =
            vtkSmartPointer<vtkPolyData>::New();
        polygonPolyData->SetPoints(points);
        polygonPolyData->SetPolys(polygons);
        mapper->SetInputData(polygonPolyData);

        actor->SetMapper(mapper);
        actor->GetProperty()->SetLineWidth(1);


        return true;
    }

    bool Updater::draw_polydata(const scrimmage_proto::Shape &s,
                                vtkSmartPointer<vtkActor> &actor,
                                vtkSmartPointer<vtkPolyDataMapper> &mapper)
    {
        vtkSmartPointer<vtkPoints> points =
            vtkSmartPointer<vtkPoints>::New();

        for (int i = 0; i < s.point_size(); i++) {
            points->InsertNextPoint(s.point(i).x(), s.point(i).y(),
                                    s.point(i).z());
        }

        vtkSmartPointer<vtkPolyData> polyData =
            vtkSmartPointer<vtkPolyData>::New();
        polyData->SetPoints(points);

        vtkSmartPointer<vtkVertexGlyphFilter> glyphFilter =
            vtkSmartPointer<vtkVertexGlyphFilter>::New();

        glyphFilter->SetInputData(polyData);
        glyphFilter->Update();

        mapper->SetInputConnection(glyphFilter->GetOutputPort());
        actor->SetMapper(mapper);
        actor->GetProperty()->SetLineWidth(1);

        return true;
    }

    bool Updater::draw_pointcloud(const scrimmage_proto::Shape &s,
                             vtkSmartPointer<vtkActor> &actor,
                             vtkSmartPointer<vtkPolyDataMapper> &mapper)
    {
        vtkSmartPointer<vtkPoints> points =
            vtkSmartPointer<vtkPoints>::New();

        for (int i = 0; i < s.point_size(); i++) {
            points->InsertNextPoint(s.point(i).x(), s.point(i).y(),
                                    s.point(i).z());
        }

        vtkSmartPointer<vtkUnsignedCharArray> color_array =
            vtkSmartPointer<vtkUnsignedCharArray>::New();
        color_array->SetName ("Colors");
        color_array->SetNumberOfComponents(3);

        if (s.point_size() != s.point_color_size()) {
            // use same color if the colors and points aren't the same size
            unsigned char c[3] = {(unsigned char)(s.color().r()),
                                  (unsigned char)(s.color().g()),
                                  (unsigned char)(s.color().b())};
            for (int i = 0; i < s.point_size(); i++) {
#if VTK_MAJOR_VERSION <= 6
                color_array->InsertNextTupleValue(c);
#else
                color_array->InsertNextTypedTuple(c);
#endif
            }
        } else {
            // Use the color vector if it's the same size as the points
            for (int i = 0; i < s.point_color_size(); i++) {
                unsigned char c[3] = {(unsigned char)(s.point_color(i).r()),
                                      (unsigned char)(s.point_color(i).g()),
                                      (unsigned char)(s.point_color(i).b())};
#if VTK_MAJOR_VERSION <= 6
                color_array->InsertNextTupleValue(c);
#else
                color_array->InsertNextTypedTuple(c);
#endif
            }
        }

        vtkSmartPointer<vtkPolyData> pointsPolydata =
            vtkSmartPointer<vtkPolyData>::New();

        pointsPolydata->SetPoints(points);

        vtkSmartPointer<vtkVertexGlyphFilter> vertexFilter =
            vtkSmartPointer<vtkVertexGlyphFilter>::New();

        vertexFilter->SetInputData(pointsPolydata);
        vertexFilter->Update();

        vtkSmartPointer<vtkPolyData> poly_data =
            vtkSmartPointer<vtkPolyData>::New();
        poly_data->ShallowCopy(vertexFilter->GetOutput());
        poly_data->GetPointData()->SetScalars(color_array);
        mapper->SetInputData(poly_data);
        actor->SetMapper(mapper);
        actor->GetProperty()->SetPointSize(s.size());

        return true;
    }

    bool Updater::draw_plane(const scrimmage_proto::Shape &s,
                             vtkSmartPointer<vtkActor> &actor,
                             vtkSmartPointer<vtkPolyDataMapper> &mapper)
    {
        // TODO: NOT REALLY WORKING YET...
        vtkSmartPointer<vtkPlaneSource> planeSource =
            vtkSmartPointer<vtkPlaneSource>::New();
        planeSource->SetCenter(s.center().x(), s.center().y(), s.center().z());
        planeSource->SetNormal(s.normal().x(), s.normal().y(), s.normal().z());
        planeSource->Update();

        vtkPolyData* plane_polydata = planeSource->GetOutput();
        mapper->SetInputData(plane_polydata);
        actor->SetMapper(mapper);

        return true;
    }

    bool Updater::draw_sphere(const scrimmage_proto::Shape &s,
                              vtkSmartPointer<vtkActor> &actor,
                              vtkSmartPointer<vtkPolyDataMapper> &mapper)
    {
        vtkSmartPointer<vtkSphereSource> sphereSource =
            vtkSmartPointer<vtkSphereSource>::New();

        sphereSource->SetCenter(0,0,0); // actor is moved later
        sphereSource->SetRadius(s.radius());

        mapper->SetInputConnection(sphereSource->GetOutputPort());

        actor->SetMapper(mapper);
        actor->SetPosition(s.center().x(), s.center().y(), s.center().z());
        return true;
    }

    bool Updater::draw_circle(const scrimmage_proto::Shape &s,
                              vtkSmartPointer<vtkActor> &actor,
                              vtkSmartPointer<vtkPolyDataMapper> &mapper)
    {
        // Create a circle
        vtkSmartPointer<vtkRegularPolygonSource> polygonSource =
            vtkSmartPointer<vtkRegularPolygonSource>::New();

        //polygonSource->GeneratePolygonOff(); // Uncomment this line to generate only the outline of the circle
        polygonSource->SetNumberOfSides(30);
        polygonSource->SetRadius(s.radius());

        mapper->SetInputConnection(polygonSource->GetOutputPort());

        actor->SetMapper(mapper);
        actor->SetPosition(s.center().x(), s.center().y(), s.center().z());
        return true;
    }

    bool Updater::draw_text(const scrimmage_proto::Shape &s,
                            vtkSmartPointer<vtkActor> &actor,
                            vtkSmartPointer<vtkPolyDataMapper> &mapper)
    {
        // Add the object label
        vtkSmartPointer<vtkVectorText> textSource =
            vtkSmartPointer<vtkVectorText>::New();
        textSource->SetText(s.text().c_str());

        mapper->SetInputConnection( textSource->GetOutputPort() );

        // Create a subclass of vtkActor: a vtkFollower that remains facing the
        // camera
        actor = vtkSmartPointer<vtkFollower>::New();
        actor->SetMapper(mapper);

        actor->SetPosition(s.center().x(), s.center().y(),
                           s.center().z());

        // We created this actor object in this function, so we can use a
        // static_cast. Need to extract the raw pointer from vtk's smartpointer
        // system first.
        static_cast<vtkFollower*>(&*actor)->SetCamera(renderer_->GetActiveCamera());

        return true;
    }
}
