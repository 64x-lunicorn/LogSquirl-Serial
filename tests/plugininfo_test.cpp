/*
 * Copyright (C) 2026 LogSquirl Contributors
 *
 * This file is part of logsquirl-serial.
 *
 * logsquirl-serial is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * logsquirl-serial is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with logsquirl-serial.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file plugininfo_test.cpp
 * @brief BDD tests verifying the plugin's C ABI metadata.
 *
 * Ensures logsquirl_plugin_get_info() returns a well-formed
 * LogSquirlPluginInfo struct with the expected field values.
 */

#include <catch2/catch.hpp>

#include "logsquirl_plugin_api.h"

#include <cstring>

// The symbol is exported from plugin.cpp (linked into the test binary).
extern "C" const LogSquirlPluginInfo* logsquirl_plugin_get_info( void );

SCENARIO( "logsquirl_plugin_get_info returns valid metadata", "[plugininfo]" )
{
    GIVEN( "the exported get_info function" )
    {
        const auto* info = logsquirl_plugin_get_info();

        THEN( "the returned pointer is not null" )
        {
            REQUIRE( info != nullptr );
        }

        WHEN( "checking the plugin id" )
        {
            THEN( "it matches the expected reverse-domain identifier" )
            {
                REQUIRE( std::strcmp( info->id, "io.github.logsquirl.serial" ) == 0 );
            }
        }

        WHEN( "checking the display name" )
        {
            THEN( "it is 'Serial Monitor'" )
            {
                REQUIRE( std::strcmp( info->name, "Serial Monitor" ) == 0 );
            }
        }

        WHEN( "checking the version" )
        {
            THEN( "it follows semantic versioning" )
            {
                REQUIRE( std::strcmp( info->version, "0.1.0" ) == 0 );
            }
        }

        WHEN( "checking the plugin type" )
        {
            THEN( "it is a UI plugin" )
            {
                REQUIRE( info->type == LOGSQUIRL_PLUGIN_UI );
            }
        }

        WHEN( "checking the API version" )
        {
            THEN( "it matches the current API version" )
            {
                REQUIRE( info->api_version == LOGSQUIRL_PLUGIN_API_VERSION );
            }
        }

        WHEN( "checking the license" )
        {
            THEN( "it is GPL-3.0-or-later" )
            {
                REQUIRE( std::strcmp( info->license, "GPL-3.0-or-later" ) == 0 );
            }
        }
    }
}
