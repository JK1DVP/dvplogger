# tool to convert zLog cty.dat to cty.cpp to use with DVPlogger
set infn "/home/araki/esp/jk1dvplog/jk1dvplog_c/jk1dvplog3/cty.dat"
set inf [open $infn r]
# read file

# struct cty_item {
#    const char *entity_desc,
#    const char *cqzone,
#    const char *ituzone,
#    const char *lat,
#    const char *lon,
#    const char *tz,
#    const char *entity,
# //   const char *prefixes
#  };
# const struct cty_item cty_list[num]=; // contains all cty information
set stat 0

set prefix_fwd_match_list ""
set prefix_exact_match_list ""
set entity_list ""
set prefix_ovrride_list ""
#
incr entity_index 0
set prefix_index 0
set entity_desc  ""
set cqzone ""
set ituzone ""
set continent ""
set lat ""
set lon ""
set tz ""
set entity ""
set prefixes ""

while { [gets $inf line] >= 0 } {
    for { set i 0 } { $i < [string length $line] } { incr i 1 } {
	set c [string index $line $i ]
	if { $c == ";" } {
	    # end of entity information
	    # print information
	    set prefix_index 0
	    set entity_desc  [string trim $entity_desc ]
	    set cqzone [string trim $cqzone ]
	    set ituzone [string trim $ituzone ]
	    set continent [string trim $continent ]
	    set lat [string trim $lat ]
	    set lon [string trim $lon ]
	    set tz [string trim $tz ]
	    set entity [string trim $entity ]
	    set prefixes [string trim $prefixes ]


#	    puts "\n$entity_index : $entity_desc $cqzone $ituzone $continent $lat $lon $tz $entity"
#	    puts " $prefixes"
	    
	    # set entity information
	    lappend entity_list $entity
	    set entity_desc_info($entity) $entity_desc
	    set cqzone_info($entity) $cqzone
	    set ituzone_info($entity) $ituzone
	    set continent_info($entity) $continent	    
	    set lat_info($entity) $lat
	    set lon_info($entity) $lon
	    set tz_info($entity) $tz

	    ### set  prefix
	    set prefixes [split $prefixes "," ]
	    foreach prefix $prefixes {
		set ovrride_index [string first "<" $prefix ]
		if { $ovrride_index == -1 } {
		    set ovrride_index [string first "(" $prefix ]
		} 
		if { $ovrride_index == -1 } {
		    set ovrride_index [string first "\{" $prefix ]
		}
		if { $ovrride_index == -1 } {
		    set ovrride_index [string first "\[" $prefix ]
		} 
		if { $ovrride_index == -1 } {
		    set ovrride_index [string first "~" $prefix ]
		}
		if { $ovrride_index != -1 } {
		    set prefix_ovrride [string range $prefix $ovrride_index end]
		    set prefix [string range $prefix 0 [expr $ovrride_index -1]]
#		    puts "$prefix $prefix_ovrride"		    
		} else {
		    set prefix_ovrride ""
		}
		if { [string index $prefix 0] == "=" } {
		    set prefix [string range $prefix 1 end]
		    # check uniqueness
		    if { [lsearch -exact $prefix_exact_match_list $prefix ] == -1 } {
			lappend prefix_exact_match_list $prefix
		    }
		} else {
		    if { [lsearch -exact $prefix_fwd_match_list $prefix ] == -1 } {		    
			lappend prefix_fwd_match_list $prefix
		    }
		}

		

		# 
		
		# check if prefix_ovrride is in prefix_ovrride_list
		if { $prefix_ovrride != "" } {
#		    puts "prefix ovrride $prefix_ovrride"
		    if { [set prefix_ovrride_index [lsearch -exact $prefix_ovrride_list $prefix_ovrride  ]] == -1 } {
#			puts "not found in the list $prefix_ovrride_list $prefix_ovrride_index"
			lappend prefix_ovrride_list $prefix_ovrride
			set prefix_ovrride_index [lsearch  -exact $prefix_ovrride_list $prefix_ovrride ]
		    }
		    set prefix_ovrride_info($prefix) $prefix_ovrride_index
		} else {
		    set prefix_ovrride_info($prefix) -1
		}
		set prefix_entity_info($prefix) $entity_index
	    }
	    
	    incr entity_index 1
	    set prefix_index 0
	    set entity_desc  ""
	    set cqzone ""
	    set ituzone ""
	    set continent ""
	    set lat ""
	    set lon ""
	    set tz ""
	    set entity ""
	    set prefixes ""
	    set stat 0
	    continue
	}
	if { $c == ":" } {
	    incr stat 1
	    continue
	}
	if { $c == "\n" } {
	    continue
	}
	if { $c == "\r" } {
	    continue
	}
	if { $c == "\t" } {
	    continue
	}
	
	switch $stat {
	    0 {
		append entity_desc $c
	    }
	    1 {
		append cqzone $c
	    }
	    2 {
		append ituzone $c
	    }
	    3 {
		append continent $c
	    }
	    4 {
		append lat $c
	    }
	    5 {
		append lon $c
	    }
	    6 {
		append tz $c
	    }
	    7 {
		append entity $c
	    }
	    8 {
		if { $c!= " " } {
		    append prefixes $c
		}
	    }
	}
    }
}

close $inf
#puts "$entity_index : $entity_desc $cqzone $ituzone $lat $lon $tz $entity"
#puts " $prefixes"

### sort prefix

proc prefix_compare {a b} {
    # compare the first character
    set cmp [ string compare [string index $a 0] [string index $b 0]]
    if { $cmp != 0 } {
	return $cmp
    }
    # length comparison
    set lena [string length $a ]
    set lenb [string length $b ]
    if { $lena > $lenb } {
	return -1
    } else {
	if { $lena < $lenb } {
	    return 1
	} else {
	    # equal length -> compare all string
	    return [string compare $a $b ]
	}
    }
}
	
set prefix_fwd_match_list [lsort -command prefix_compare $prefix_fwd_match_list ]
set prefix_exact_match_list [lsort $prefix_exact_match_list ]

#puts "fwd_match $prefix_fwd_match_list"
#puts "exact_match $prefix_exact_match_list"

#puts "prefix override list $prefix_ovrride_list"


### print entity information

puts "struct entity_info \{"
puts " const char *entity; "
puts " const char *entity_desc;"
puts " const char *cqzone;"
puts " const char *ituzone;"
puts " const char *continent;"
puts " const char *lat;"
puts " const char *lon;"
puts " const char *tz;"
puts "\};\n"

puts "const struct entity_info entity_infos\[[expr [llength $entity_list]+1]\]=\{"
foreach entity $entity_list {
    puts " \{ \"$entity\",\"$entity_desc_info($entity)\",\"$cqzone_info($entity)\",\"$ituzone_info($entity)\",\"$continent_info($entity)\",\"$lat_info($entity)\",\"$lon_info($entity)\",\"$tz_info($entity)\" \},"
}
puts  "\{ \"\",\"\",\"\",\"\",\"\",\"\",\"\",\"\" \}\n\};\n"
    
puts "const char *cty_ovrride_info\[[expr [llength $prefix_ovrride_list]+1]\]={"
foreach ovrride $prefix_ovrride_list  {
    puts "  \"$ovrride\", "
}
puts "  \"\"\n};"

### split prefix_fwd_match_list by prefix length 
foreach prefix $prefix_fwd_match_list {
    set len [string length $prefix ]
    if { [info exists prefix_fwd_match_list_nchar($len) ] } {
	lappend prefix_fwd_match_list_nchar($len) $prefix
    } else {
	set prefix_fwd_match_list_nchar($len) $prefix
    }
}


### prefix listing 
puts "struct prefix_list \{"
puts "    const char *prefix; "
puts "    const int entity_idx; "
puts "    const int ovrride_info_idx; "
puts "\};"
puts "const struct prefix_list prefix_fwd_match_list\[[expr [llength $prefix_fwd_match_list]+1]\] = \{"

# make index list of exact match by the first character
set idx_chars "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
for { set i 0 } { $i < [string length $idx_chars ] } {incr i 1 } {
    set idx_char [string index $idx_chars $i ] 
    set prefix_fwd_match_list_idx($idx_char) -1
    set prefix_fwd_match_list_idx_end($idx_char) -1    
}

set prefix_idx 0
set idx_char_prev ""
foreach prefix $prefix_fwd_match_list {
    set entity_index $prefix_entity_info($prefix)
    set entity [lindex $entity_list $entity_index ]
    set idx_char [string index $prefix 0]
    
    if { $prefix_fwd_match_list_idx($idx_char) == -1 } {
	set prefix_fwd_match_list_idx($idx_char) $prefix_idx
    }
    if { $idx_char_prev != $idx_char } {
	if { $idx_char_prev != "" } {
	    set prefix_fwd_match_list_idx_end($idx_char_prev) [expr $prefix_idx -1 ]
	}
    }
    set idx_char_prev $idx_char

    #    puts -nonewline "prefix $prefix : $entity $entity_desc_info($entity)"    
    if { $prefix_ovrride_info($prefix) != -1 } {
	#puts " $prefix_ovrride_info($prefix) "
	#	puts " [lindex $prefix_ovrride_list $prefix_ovrride_info($prefix) ]"
	puts " \{ \"$prefix\", $entity_index , $prefix_ovrride_info($prefix) \},"
    } else {
	puts " \{ \"$prefix\", $entity_index , -1 \},"
#	puts ""
    }
    incr prefix_idx 1    
}
set prefix_fwd_match_list_idx_end($idx_char) [expr $prefix_idx -1 ]
puts " \{ \"\",-1,-1 \}\n\};\n"


puts "// order of $idx_chars"
puts "int prefix_fwd_match_list_idx\[[string length $idx_chars]\]\[2\]= \{"
for { set i 0 } { $i < [string length $idx_chars ] } {incr i 1 } {
    set idx_char [string index $idx_chars $i ]
    puts "\{ $prefix_fwd_match_list_idx($idx_char), $prefix_fwd_match_list_idx_end($idx_char) \},"
}
puts "\};"


### exact match information
# make index list of exact match by the first character

for { set i 0 } { $i < [string length $idx_chars ] } {incr i 1 } {
    set idx_char [string index $idx_chars $i ] 
    set prefix_exact_match_list_idx($idx_char) -1
    set prefix_exact_match_list_idx_end($idx_char) -1    
}

puts "const struct prefix_list prefix_exact_match_list\[[expr [llength $prefix_exact_match_list]+1]\] = \{"
set prefix_idx 0
set idx_char_prev ""
foreach prefix $prefix_exact_match_list {
    set entity_index $prefix_entity_info($prefix)
    set entity [lindex $entity_list $entity_index ]
    set idx_char [string index $prefix 0]
    if { $prefix_exact_match_list_idx($idx_char) == -1 } {
	set prefix_exact_match_list_idx($idx_char) $prefix_idx
    }
    if { $idx_char_prev != $idx_char } {
	if { $idx_char_prev != "" } {
	    set prefix_exact_match_list_idx_end($idx_char_prev) [expr $prefix_idx -1 ]
	}
    }
    set idx_char_prev $idx_char
    
#    puts -nonewline "exact prefix $prefix : $entity $entity_desc_info($entity)"
    if { $prefix_ovrride_info($prefix) != -1 } {
#	puts " [lindex $prefix_ovrride_list $prefix_ovrride_info($prefix) ]"
	puts " \{ \"$prefix\", $entity_index , $prefix_ovrride_info($prefix) \},"	
    } else {
#	puts ""
	puts " \{ \"$prefix\", $entity_index , -1 \},"	
    }
    incr prefix_idx 1
}
set prefix_exact_match_list_idx_end($idx_char) [expr $prefix_idx -1 ]
puts " \{ \"\",-1,-1 \}\n\};\n"
puts "// order of $idx_chars"
puts "int prefix_exact_match_list_idx\[[string length $idx_chars]\]\[2\]= \{"
for { set i 0 } { $i < [string length $idx_chars ] } {incr i 1 } {
    set idx_char [string index $idx_chars $i ]
    puts "\{$prefix_exact_match_list_idx($idx_char),$prefix_exact_match_list_idx_end($idx_char)\},"
#    set prefix_exact_match_list_idx($idx_char) -1
}
puts "\};"

# first line  entity_desc cqzone ituzone continent lat lon timezone(+utc) entity  with : separator
# second line prefix_search separated by comma   a country description is terminated by ;
	# search prefix has special rule such an example as K4CU(4)[8]  
	
