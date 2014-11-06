# C header file generator that automatically escaped newlines. This is meant to
# process header files containing DTrace related macros, removing the need to
# manually escape every newline in said files.
class DtraceHeaderGenerator
  def initialize(target)
    @target = target
  end

  # Processes the given header files and saves the resulting header as the file
  # specified in @target.
  def generate(headers)
    contents = ''

    headers.each do |header_file|
      File.open(header_file, 'r') do |handle|
        handle.each_line do |line|
          contents << "#{line.chomp} \\\n"
        end
      end

      contents << "\n"
    end

    File.open(@target, 'w') do |handle|
      handle.write <<-EOF.strip
#ifndef RUBINIUS_DTRACE_HOOKS_H
#define RUBINIUS_DTRACE_HOOKS_H

#{contents}

#endif
      EOF
    end
  end
end
