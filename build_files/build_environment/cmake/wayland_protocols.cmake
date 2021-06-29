# ***** BEGIN GPL LICENSE BLOCK *****
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#
# ***** END GPL LICENSE BLOCK *****

# Note the utility apps may use png/tiff/gif system libraries, but the
# library itself does not depend on them, so should give no problems.

ExternalProject_Add(external_wayland_protocols
  URL file://${PACKAGE_DIR}/${WL_PROTOCOLS_FILE}
  DOWNLOAD_DIR ${DOWNLOAD_DIR}
  URL_HASH ${WL_PROTOCOLS_HASH_TYPE}=${WL_PROTOCOLS_HASH}
  PREFIX ${BUILD_DIR}/wayland-protocols
  CONFIGURE_COMMAND meson --prefix ${LIBDIR}/wayland-protocols . ../external_wayland_protocols ${DEFAULT_CMAKE_FLAGS}
  BUILD_COMMAND ninja
  INSTALL_COMMAND ninja install
)

ExternalProject_Add_Step(external_wayland_protocols after_install
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${LIBDIR}/wayland-protocols ${HARVEST_TARGET}/wayland-protocols
  DEPENDEES install
)

list(APPEND CMAKE_PREFIX_PATH "${HARVEST_TARGET}/wayland-protocols/share/pkgconfig/")
