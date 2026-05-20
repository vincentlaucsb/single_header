#include "amalgamator.hpp"
#include "cli.hpp"
#include "helpers.hpp"

#include <exception>
#include <iostream>

int main(int argc, char **argv) {
  try {
    const auto options = single_header::parse_args(argc, argv);
    if (options.show_help) {
      std::cout << single_header::usage();
      return 0;
    }

    single_header::write_text(options.output, single_header::generate(options));
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "single_header: " << error.what() << "\n\n" << single_header::usage();
    return 1;
  }
}
