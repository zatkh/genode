/*
 * \brief  Report server that writes reports to file-systems
 * \author Emery Hemingway
 * \date   2017-05-19
 */

/*
 * Copyright (C) 2017 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

#include <vfs/dir_file_system.h>
#include <vfs/file_system_factory.h>
#include <os/path.h>
#include <report_session/report_session.h>
#include <root/component.h>
#include <base/attached_rom_dataspace.h>
#include <base/attached_ram_dataspace.h>
#include <base/session_label.h>
#include <base/heap.h>
#include <base/component.h>
#include <util/arg_string.h>

namespace Fs_report {
	using namespace Genode;
	using namespace Report;
	using namespace Vfs;

	class  Session_component;
	class  Root;
	struct Main;

	typedef Genode::Path<Session_label::capacity()> Path;

static bool create_parent_dir(Vfs::Directory_service &vfs, Path const &child)
{
	typedef Vfs::Directory_service::Mkdir_result Mkdir_result;

	Path parent = child;
	parent.strip_last_element();

	Mkdir_result res = vfs.mkdir(parent.base(), 0);
	if (res == Mkdir_result::MKDIR_ERR_NO_ENTRY) {
		if (!create_parent_dir(vfs, parent))
			return false;
		res = vfs.mkdir(parent.base(), 0);
	}

	switch (res) {
	case Mkdir_result::MKDIR_OK:
	case Mkdir_result::MKDIR_ERR_EXISTS:
		 return true;
	default:
		return false;
	}
}

}

class Fs_report::Session_component : public Genode::Rpc_object<Report::Session>
{
	private:

		Path _leaf_path;

		Attached_ram_dataspace _ds;

		Vfs_handle *_handle;
		file_size   _file_size = 0;
		bool        _success = true;

	public:

		Session_component(Genode::Env                 &env,
		                  Genode::Allocator           &alloc,
		                  Vfs::File_system            &vfs,
		                  Genode::Session_label const &label,
		                  size_t                       buffer_size)
		: _ds(env.ram(), env.rm(), buffer_size)
		{
			typedef Vfs::Directory_service::Open_result Open_result;

			Path path = path_from_label<Path>(label.string());
			path.append(".report");

			create_parent_dir(vfs, path);

			Open_result res = vfs.open(
				path.base(),
				Directory_service::OPEN_MODE_WRONLY |
					Directory_service::OPEN_MODE_CREATE,
				&_handle, alloc);

			if (res == Open_result::OPEN_ERR_EXISTS) {
				res = vfs.open(
					path.base(),
					Directory_service::OPEN_MODE_WRONLY,
					&_handle, alloc);
				if (res == Open_result::OPEN_OK)
					_handle->fs().ftruncate(_handle, 0);
			}

			if (res != Open_result::OPEN_OK) {
				error("failed to open '", path, "'");
				throw Service_denied();
			}

			/* get the leaf path from the leaf file-system */
			if (char const *leaf_path = _handle->ds().leaf_path(path.base()))
				_leaf_path.import(leaf_path);
		}

		~Session_component()
		{
			if (_handle)
				_handle->ds().close(_handle);
		}

		Dataspace_capability dataspace() override { return _ds.cap(); }

		void submit(size_t const length) override
		{
			/* TODO: close and reopen on error */

			typedef Vfs::File_io_service::Write_result Write_result;

			if (_file_size != length)
				_handle->fs().ftruncate(_handle, length);

			size_t offset = 0;
			while (offset < length) {
				file_size n = 0;

				_handle->seek(offset);
				Write_result res = _handle->fs().write(
					_handle, _ds.local_addr<char const>(),
					length - offset, n);

				if (res != Write_result::WRITE_OK) {
					/* do not spam the log */
					if (_success)
						error("failed to write report to '", _leaf_path, "'");
					_file_size = 0;
					_success = false;
					return;
				}

				offset += n;
			}

			_file_size = length;
			_success = true;

			/* flush to notify watchers */
			_handle->ds().sync(_leaf_path.base());
		}

		void response_sigh(Genode::Signal_context_capability) override { }

		size_t obtain_response() override { return 0; }
};


class Fs_report::Root : public Genode::Root_component<Session_component>
{
	private:

		Genode::Env  &_env;
		Genode::Heap  _heap { &_env.ram(), &_env.rm() };

		Genode::Attached_rom_dataspace _config_rom { _env, "config" };

		Genode::Xml_node vfs_config()
		{
			try { return _config_rom.xml().sub_node("vfs"); }
			catch (...) {
				Genode::error("VFS not configured");
				_env.parent().exit(~0);
				throw;
			}
		}

		struct Io_dummy : Io_response_handler {
			void handle_io_response(Vfs::Vfs_handle::Context*) override { }
		} _io_response_handler { };

		Vfs::Global_file_system_factory _global_file_system_factory { _heap };

		Vfs::Dir_file_system _vfs {
			_env, _heap, vfs_config(),
			_io_response_handler,
			_global_file_system_factory };

		Genode::Signal_handler<Root> _config_dispatcher {
			_env.ep(), *this, &Root::_config_update };

		void _config_update()
		{
			_config_rom.update();
			_vfs.apply_config(vfs_config());
		}

	protected:

		Session_component *_create_session(const char *args) override
		{
			using namespace Genode;

			/* read label from session arguments */
			Session_label const label = label_from_args(args);

			/* read RAM donation from session arguments */
			size_t const ram_quota =
				Arg_string::find_arg(args, "ram_quota").aligned_size();
			/* read report buffer size from session arguments */
			size_t const buffer_size =
				Arg_string::find_arg(args, "buffer_size").aligned_size();

			size_t session_size =
				max((size_t)4096, sizeof(Session_component)) +
				buffer_size;

			if (session_size > ram_quota) {
				error("insufficient 'ram_quota' from '", label, "' "
				      "got ", ram_quota, ", need ", session_size);
				throw Insufficient_ram_quota();
			}

			return new (md_alloc())
				Session_component(_env, _heap, _vfs, label, buffer_size);
		}

	public:

		Root(Genode::Env &env, Genode::Allocator &md_alloc)
		:
			Genode::Root_component<Session_component>(env.ep(), md_alloc),
			_env(env)
		{ }
};


struct Fs_report::Main
{
	Env &env;

	Sliced_heap sliced_heap { env.ram(), env.rm() };

	Root root { env, sliced_heap };

	Main(Env &env) : env(env)
	{
		env.parent().announce(env.ep().manage(root));
	}
};

void Component::construct(Genode::Env &env) { static Fs_report::Main main(env); }
