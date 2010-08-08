# Copyright (C) 2010  Kouhei Sutou <kou@clear-code.com>
#
# This library is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this library.  If not, see <http://www.gnu.org/licenses/>.

require 'English'
require 'milter/manager/condition-table'
require 'milter/manager/postfix-condition-table-parser'

module Milter::Manager
  class PostfixRegexpTable
    include ConditionTable
    include PostfixConditionTableParser

    def initialize
      @table = []
    end

    def parse(io)
      current_table = @table
      tables = []
      each_line(io) do |line|
        case line
        when /\A\s*if\s+(!)?\/(.*)\/([imx]+)?$/
          not_flag = $1
          pattern = $2
          flag = $3
          regexp_flag = Regexp::IGNORECASE
          if flag
            regexp_flag &= Regexp::IGNORECASE if flag.index("i")
            regexp_flag |= Regexp::MULTILINE if flag.index("m")
          end
          regexp = nil
          begin
            regexp = Regexp.new(pattern, regexp_flag)
          rescue RegexpError
            raise InvalidValueError.new(pattern, $!.message, line,
                                        io.path, io.lineno)
          end
          new_table = []
          current_table << [not_flag == "!", regexp, new_table]
          current_table = new_table
          tables << new_table
        when /\A\s*(!)?\/(.*)\/([imx]+)?\s+(.+)\s*$/
          not_flag = $1
          pattern = $2
          flag = $3
          action = $4
          regexp_flag = Regexp::IGNORECASE
          if flag
            regexp_flag &= Regexp::IGNORECASE if flag.index("i")
            regexp_flag |= Regexp::MULTILINE if flag.index("m")
          end
          regexp = nil
          begin
            regexp = Regexp.new(pattern, regexp_flag)
          rescue RegexpError
            raise InvalidValueError.new(pattern, $!.message, line,
                                        io.path, io.lineno)
          end
          current_table << [not_flag == "!", regexp, action]
        when /\Aendif\s*$/
          current_table = tables.pop
        else
          raise InvalidFormatError.new(line, io.path, io.lineno)
        end
      end
      unless tables.empty?
        raise InvalidFormatError("endif isn't matched", io.path, io.lineno)
      end
    end

    def find(text)
      find_action(@table, text)
     end

    private
    def find_action(table, text)
      table.each do |negative, regexp, table_or_action|
        if negative
          next if regexp =~ text
        else
          next if regexp !~ text
        end
        if table_or_action.is_a?(Array)
          action = find_action(table_or_action, text)
          return action if action
        else
          return table_or_action
        end
      end
      nil
    end
  end
end
