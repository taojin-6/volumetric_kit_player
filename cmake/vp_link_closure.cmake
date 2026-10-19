# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Tao Jin

# `vp_check_not_linked(<target> <forbidden>)` fails the configure when
# <forbidden> is in <target>'s link closure: the items it links, directly or
# through any target among them. Aliases resolve to their targets, and the
# $<BUILD_INTERFACE:...> and $<LINK_ONLY:...> wrappers to what they wrap; other
# generator expressions are not followed. <forbidden> is matched by name, so an
# imported target another directory found, which is not visible here (as gfx's
# glfw is not), is caught too, though not followed.
function(vp_check_not_linked target forbidden)
  set(seen "")
  set(pending "${target}")
  while(pending)
    list(POP_FRONT pending item)
    string(REGEX REPLACE "^\\$<(BUILD_INTERFACE|LINK_ONLY):(.*)>$" "\\2" item
                         "${item}")
    if(TARGET "${item}")
      get_target_property(aliased "${item}" ALIASED_TARGET)
      if(aliased)
        set(item "${aliased}")
      endif()
    endif()
    if(item STREQUAL forbidden)
      message(FATAL_ERROR "${target} links ${forbidden} (DECISIONS.md)")
    endif()
    if(NOT TARGET "${item}" OR item IN_LIST seen)
      continue()
    endif()
    list(APPEND seen "${item}")
    set(properties INTERFACE_LINK_LIBRARIES)
    get_target_property(type "${item}" TYPE)
    if(NOT type STREQUAL "INTERFACE_LIBRARY")
      list(APPEND properties LINK_LIBRARIES)
    endif()
    foreach(property IN LISTS properties)
      get_target_property(links "${item}" ${property})
      if(links)
        list(APPEND pending ${links})
      endif()
    endforeach()
  endwhile()
endfunction()
