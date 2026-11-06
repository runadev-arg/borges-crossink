#pragma once

// What the transfer servers never hand out, whatever Show Hidden Files says: Borges keeps its sealed
// credential and its queue there, and the old firmware's folder has a token any MAC can read. That
// setting is about a person managing their files, not about giving them to whoever joined the
// network, and these have no auth in front of them.
//
// Paths as a client sends them: FAT does not care about case, and takes the short name too.

namespace borges {
namespace glue {

bool isBorgesPrivatePath(const char* path);

}  // namespace glue
}  // namespace borges
