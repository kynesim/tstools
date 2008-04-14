# Pseudo-Python rendition of the code for ``get_next_access_unit()``.

# ***** BEGIN LICENSE BLOCK *****
# Version: MPL 1.1
# 
# The contents of this file are subject to the Mozilla Public License Version
# 1.1 (the "License"); you may not use this file except in compliance with
# the License. You may obtain a copy of the License at
# http://www.mozilla.org/MPL/
# 
# Software distributed under the License is distributed on an "AS IS" basis,
# WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
# for the specific language governing rights and limitations under the
# License.
# 
# The Original Code is the MPEG TS, PS and ES tools.
# 
# The Initial Developer of the Original Code is Amino Communications Ltd.
# Portions created by the Initial Developer are Copyright (C) 2008
# the Initial Developer. All Rights Reserved.
# 
# Contributor(s):
#   Amino Communications Ltd, Swavesey, Cambridge UK
# 
# ***** END LICENSE BLOCK *****

def get_next_access_unit(context):
    """Retrieve the next access unit from the file described by `context`.
    """
    access_unit = build_access_unit()
    if context.pending_nal: # i.e., we already had a NAL to start this unit
        access_unit.append(context.pending_nal,TRUE,context.pending_list)
        context.pending_nal = NULL
        context.pending_list.reset(FALSE)
    
    while 1:
        try:
            nal = context.find_next_NAL_unit()
        except EOF:
            context.no_more_data = TRUE; # prevent future reads on this stream
            break
        except BrokenNALUnit:
            WARNING("!!! Ignoring broken NAL unit\n")
            access_unit.ignored_broken_NAL_units += 1
            continue

        if nal.is_slice():
            if not access_unit.started_primary_picture:
                # We're in a new access unit, but we haven't had a slice
                # yet, so we can be lazy and assume that this must be the
                # first slice
                nal.start_reason = "First slice of new access unit"
                access_unit.append(nal,TRUE,context.pending_list)
                context.pending_list.reset(FALSE)
                context.remember_earlier_primary_start(nal)
            elif nal.is_first_VCL_NAL(context.earlier_primary_start):
                # Regardless of what we determine next, we need to remember
                # that the NAL started (what may later be the previous) access
                # unit
                context.remember_earlier_primary_start(nal)
                if access_unit.started_primary_picture:
                    # We were already in an access unit with a primary
                    # picture, so this NAL unit must start a new access unit.
                    # Remember it for next time, and return the access unit so
                    # far.
                    context.pending_nal = nal
                    break;    # Ready to return the access unit
                else:
                    # This access unit was waiting for its primary picture
                    access_unit.append(nal,TRUE,context.pending_list)
                    context.pending_list.reset(FALSE)
            elif not access_unit.started_primary_picture:
                # But this is not a NAL unit that may start a new
                # access unit. So what should we do? Ignore it?
                if not quiet:
                    WARNING("!!! Ignoring VCL NAL that cannot start a new"
                            " primary picture: "
                    nal.report(stderr)
            elif nal_is_redundant(nal):
                # printf("     ignoring redundant NAL unit\n")
                pass
            else:
                # We're part of the same access unit, but not special
                access_unit.append(nal,FALSE,context.pending_list)
                context.pending_list.reset(FALSE)
        elif nal.nal_unit_type == NAL_ACCESS_UNIT_DELIM:
            # An access unit delimiter always starts a new access unit
            if access_unit.started_primary_picture:
                context.pending_list.append(nal)
                break # Ready to return the "previous" access unit
            else:
                # The current access unit doesn't yet have any VCL NALs
                if context.pending_list.length > 0:
                    WARNING("!!! Ignoring items after last VCL NAL and"
                                " before Access Unit Delimiter\n")
                    context.pending_list.report(stderr,"    ",NULL,)
                    context.pending_list.reset(TRUE)
                if access_unit.nal_units.length > 0:
                    WARNING("!!! Ignoring incomplete access unit\n")
                    access_unit.nal_units.report(stderr,"    ",NULL,)
                    access_unit.nal_units.reset(TRUE)
                access_unit.append(nal,FALSE,NULL)
        elif nal.nal_unit_type == NAL_SEI:
            # SEI units always precede the primary coded picture
            # - so they also implicitly end any access unit that has already
            # started its primary picture
            if access_unit.started_primary_picture:
                context.pending_list.append(nal)
                break # Ready to return the "previous" access unit
            else:
                context.pending_list.append(nal)
        elif nal.nal_unit_type in [NAL_SEQ_PARAM_SET, NAL_PIC_PARAM_SET,
                                   13, 14, 15, 16, 17, 18]:
            # These start a new access unit *if* they come after the last VCL
            # NAL of an access unit. But we can only *tell* that they are
            # after the last VCL NAL of an access unit when we start the next
            # access unit - so we need to hold them in hand until we know that
            # we need them.  (i.e., they'll get added to an access unit just
            # before the next "more determined" NAL unit we add to an access
            # unit)
            context.pending_list.append(nal)
        elif nal.nal_unit_type == NAL_END_OF_SEQ:
          if context.pending_list.length > 0:
            WARNING("!!! Ignoring items after last VCL NAL and"
                    " before End of Sequence\n")
            context.pending_list.report(stderr,"    ",NULL,)
            context.pending_list.reset(TRUE)
            # And remember this as the End of Sequence marker
            context.end_of_sequence = nal
            break
        elif nal.nal_unit_type == NAL_END_OF_STREAM:
            # And remember this as the End of Stream marker
            context.end_of_stream = nal
            # Which means there's no point in reading more from this stream
            # (setting no_more_data like this means that *next* time this
            # function is called, it will return EOF)
            context.no_more_data = TRUE
            # And we're done
            break
        else:
          # It's not a slice, or an access unit delimiter, or an
          # end of sequence or stream, or a sequence or picture
          # parameter set, or various other odds and ends, so it
          # looks like we can ignore it.
          pass

    # Check for an immediate "end of file with no data"
    # - i.e., we read EOF or end of stream, and there was nothing
    # between the last access unit and such reading
    if context.no_more_data and access_unit.nal_units.length == 0:
        raise EOF
    
    # Otherwise, finish off and return the access unit we have in hand
    access_unit.end(context,show_details)

    # Remember to count it
    context.access_unit_index += 1

    return access_unit
