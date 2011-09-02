
//
// ===============================================================================
// clReflect
// -------------------------------------------------------------------------------
// Copyright (c) 2011 Don Williamson
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
// of the Software, and to permit persons to whom the Software is furnished to do
// so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
// ===============================================================================
//

#include "CppExport.h"
#include "PtrRelocator.h"

#include <clReflectCore/Database.h>
#include <clReflectCore/Logging.h>
#include <clReflectCpp/DatabaseLoader.h>

#include <clcpp/Database.h>

#include <algorithm>
#include <malloc.h>


namespace
{
	// A basic malloc allocator implementation
	class Malloc : public clcpp::IAllocator
	{
		void* Alloc(unsigned int size)
		{
			return malloc(size);
		}
		void Free(void* ptr)
		{
			free(ptr);
		}
	};


	void BuildNames(const cldb::Database& db, CppExport& cppexp)
	{
		// Allocate the name data
		unsigned int name_data_size = 0;
		for (cldb::NameMap::const_iterator i = db.m_Names.begin(); i != db.m_Names.end(); ++i)
		{
			name_data_size += i->second.text.length() + 1;
		}
		cppexp.db->name_text_data = cppexp.allocator.Alloc<char>(name_data_size);

		// Populate the name data and build the sorted name map
		name_data_size = 0;
		for (cldb::NameMap::const_iterator i = db.m_Names.begin(); i != db.m_Names.end(); ++i)
		{
			char* text_ptr = (char*)(cppexp.db->name_text_data + name_data_size);
			cppexp.name_map[i->first] = text_ptr;
			const cldb::Name& name = i->second;
			strcpy(text_ptr, name.text.c_str());
			name_data_size += name.text.length() + 1;
		}

		// Build the in-memory name array
		unsigned int nb_names = cppexp.name_map.size();
		cppexp.db->names.copy(clcpp::CArray<clcpp::Name>(cppexp.allocator.Alloc<clcpp::Name>(nb_names), nb_names));
		unsigned int index = 0;
		for (CppExport::NameMap::const_iterator i = cppexp.name_map.begin(); i != cppexp.name_map.end(); ++i)
		{
			clcpp::Name name;
			name.hash = i->first;
			name.text = i->second;
			cppexp.db->names[index++] = name;
		}
	}


	// Overloads for copying between databases
	// TODO: Rewrite with database metadata?
	void CopyPrimitive(clcpp::Primitive& dest, const cldb::Primitive& src)
	{
	}
	void CopyPrimitive(clcpp::EnumConstant& dest, const cldb::EnumConstant& src)
	{
		dest.value = src.value;
	}
	void CopyPrimitive(clcpp::Function& dest, const cldb::Function& src)
	{
		dest.address = src.address;
		dest.unique_id = src.unique_id;
	}
	void CopyPrimitive(clcpp::Field& dest, const cldb::Field& src)
	{
		dest.type = (clcpp::Type*)src.type.hash;
		dest.is_const = src.is_const;
		dest.offset = src.offset;
		dest.parent_unique_id = src.parent_unique_id;
		switch (src.modifier)
		{
		case (cldb::Field::MODIFIER_VALUE): dest.modifier = clcpp::Field::MODIFIER_VALUE; break;
		case (cldb::Field::MODIFIER_POINTER): dest.modifier = clcpp::Field::MODIFIER_POINTER; break;
		case (cldb::Field::MODIFIER_REFERENCE): dest.modifier = clcpp::Field::MODIFIER_REFERENCE; break;
		default: assert(false && "Case not handled");
		}
	}
	void CopyPrimitive(clcpp::Type& dest, const cldb::Type& src)
	{
		dest.size = src.size;
	}
	void CopyPrimitive(clcpp::TemplateType& dest, const cldb::TemplateType& src)
	{
		for (int i = 0; i < cldb::TemplateType::MAX_NB_ARGS; i++)
		{
			dest.parameter_types[i] = (clcpp::Type*)src.parameter_types[i].hash;
			dest.parameter_ptrs[i] = src.parameter_ptrs[i];
		}
	}
	void CopyPrimitive(clcpp::Class& dest, const cldb::Class& src)
	{
		dest.size = src.size;
		dest.base_class = (clcpp::Class*)src.base_class.hash;
	}
	void CopyPrimitive(clcpp::IntAttribute& dest, const cldb::IntAttribute& src)
	{
		dest.value = src.value;
	}
	void CopyPrimitive(clcpp::FloatAttribute& dest, const cldb::FloatAttribute& src)
	{
		dest.value = src.value;
	}
	void CopyPrimitive(clcpp::NameAttribute& dest, const cldb::NameAttribute& src)
	{
		// Only copy the hash as the string will be patched up later
		dest.value.hash = src.value.hash;
	}
	void CopyPrimitive(clcpp::TextAttribute& dest, const cldb::TextAttribute& src)
	{
		// Store a pointer to the cldb text allocation that will be replaced later
		dest.value = src.value.c_str();
	}


	template <typename CLDB_TYPE, typename CLCPP_TYPE>
	void BuildCArray(CppExport& cppexp, clcpp::CArray<CLCPP_TYPE>& dest, const cldb::Database& db)
	{
		// Allocate enough entries for all primitives
		const cldb::PrimitiveStore<CLDB_TYPE>& src = db.GetPrimitiveStore<CLDB_TYPE>();
		dest.copy(clcpp::CArray<CLCPP_TYPE>(cppexp.allocator.Alloc<CLCPP_TYPE>(src.size()), src.size()));

		// Copy individually
		int index = 0;
		for (cldb::PrimitiveStore<CLDB_TYPE>::const_iterator i = src.begin(); i != src.end(); ++i)
		{
			// Copy as a primitive first
			CLCPP_TYPE& dest_prim = dest[index++];
			const CLDB_TYPE& src_prim = i->second;
			dest_prim.kind = CLCPP_TYPE::KIND;
			dest_prim.name.hash = src_prim.name.hash;
			dest_prim.parent = (clcpp::Primitive*)src_prim.parent.hash;

			// Early reference the text of the name for easier debugging
			dest_prim.name.text = cppexp.name_map[src_prim.name.hash];

			// Then custom
			CopyPrimitive(dest_prim, src_prim);
		}
	}


	bool ParentAndChildMatch(const clcpp::Primitive&, const clcpp::Primitive&)
	{
		return true;
	}
	bool ParentAndChildMatch(const clcpp::Function& parent, const clcpp::Field& child)
	{
		return parent.unique_id == child.parent_unique_id;
	}


	template <typename PARENT_TYPE, typename CHILD_TYPE>
	void Parent(clcpp::CArray<PARENT_TYPE>& parents, clcpp::CArray<const CHILD_TYPE*> (PARENT_TYPE::*carray), clcpp::CArray<CHILD_TYPE*>& children, StackAllocator& allocator)
	{
		// Create a lookup table from hash ID to parent and the number of references the parent has
		typedef std::pair<PARENT_TYPE*, int> ParentAndRefCount;
		typedef std::multimap<unsigned int, ParentAndRefCount> ParentMap;
		typedef std::pair<ParentMap::iterator, ParentMap::iterator> ParentMapRange;
		ParentMap parent_map;
		for (int i = 0; i < parents.size(); i++)
		{
			PARENT_TYPE& parent = parents[i];
			parent_map.insert(ParentMap::value_type(parent.name.hash, ParentAndRefCount(&parent, 0)));
		}

		// Assign parents and count the references
		for (int i = 0; i < children.size(); i++)
		{
			CHILD_TYPE* child = children[i];

			// Iterate over all matches
			unsigned int hash = (unsigned int)child->parent;
			ParentMapRange range = parent_map.equal_range(hash);
			for (ParentMap::iterator j = range.first; j != range.second; ++j)
			{
				ParentAndRefCount& parc = j->second;
				if (ParentAndChildMatch(*parc.first, *child))
				{
					child->parent = parc.first;
					parc.second++;
					break;
				}
			}
		}

		// Allocate the arrays in the parent
		for (ParentMap::iterator i = parent_map.begin(); i != parent_map.end(); ++i)
		{
			if (int nb_refs = i->second.second)
			{
				PARENT_TYPE& parent = *i->second.first;
				(parent.*carray).copy(clcpp::CArray<const CHILD_TYPE*>(allocator.Alloc<const CHILD_TYPE*>(nb_refs), nb_refs));

				// To save having to do any further lookups, store the count inside the array
				// at the end
				(parent.*carray)[nb_refs - 1] = 0;
			}
		}

		// Fill in all the arrays
		for (int i = 0; i < children.size(); i++)
		{
			CHILD_TYPE* child = children[i];
			PARENT_TYPE* parent = (PARENT_TYPE*)child->parent;

			// Only process if the parent has been correctly assigned
			if (parent >= parents.data() && parent < parents.data() + parents.size())
			{
				// Locate the current constant count at the end of the array and add this constant
				// to its parent
				int nb_constants = (parent->*carray).size();
				int cur_count = (int)(parent->*carray)[nb_constants - 1];
				(parent->*carray)[cur_count++] = child;

				// When the last constant gets written, the constant count gets overwritten with
				// the constant pointer and should no longer be updated
				if (cur_count != nb_constants)
				{
					(parent->*carray)[nb_constants - 1] = (CHILD_TYPE*)cur_count;
				}
			}
		}
	}

	template <typename PARENT_TYPE, typename CHILD_TYPE>
	void Parent(clcpp::CArray<PARENT_TYPE>& parents, clcpp::CArray<const CHILD_TYPE*> (PARENT_TYPE::*carray), clcpp::CArray<CHILD_TYPE>& children, StackAllocator& allocator)
	{
		// Create an array of pointers to the children and forward that to the Parent function
		// that acts on arrays of pointers
		Malloc malloc_allocator;
		clcpp::CArray<CHILD_TYPE*> children_ptrs(children.size(), &malloc_allocator);
		for (int i = 0; i < children_ptrs.size(); i++)
		{
			children_ptrs[i] = &children[i];
		}

		Parent(parents, carray, children_ptrs, allocator);
	}


	template <typename TYPE>
	void BuildAttributePtrArray(clcpp::CArray<clcpp::Attribute*>& dest, clcpp::CArray<TYPE>& src, int& pos)
	{
		for (int i = 0; i < src.size(); i++)
		{
			dest[pos++] = &src[i];
		}
	}


	void BuildAttributePtrArray(CppExport& cppexp, clcpp::CArray<clcpp::Attribute*>& attributes)
	{
		// Total count of all attributes
		int size =
			cppexp.db->flag_attributes.size() +
			cppexp.db->int_attributes.size() +
			cppexp.db->float_attributes.size() +
			cppexp.db->name_attributes.size() +
			cppexp.db->text_attributes.size();

		// Create the destination array
		attributes.copy(clcpp::CArray<clcpp::Attribute*>(cppexp.allocator.Alloc<clcpp::Attribute*>(size), size));

		// Collect all attribute pointers
		int pos = 0;
		BuildAttributePtrArray(attributes, cppexp.db->flag_attributes, pos);
		BuildAttributePtrArray(attributes, cppexp.db->int_attributes, pos);
		BuildAttributePtrArray(attributes, cppexp.db->float_attributes, pos);
		BuildAttributePtrArray(attributes, cppexp.db->name_attributes, pos);
		BuildAttributePtrArray(attributes, cppexp.db->text_attributes, pos);
	}


	void AssignAttributeNamesAndText(CppExport& cppexp)
	{
		// First assign the name value text pointers
		for (int i = 0; i < cppexp.db->name_attributes.size(); i++)
		{
			clcpp::NameAttribute& attr = cppexp.db->name_attributes[i];
			attr.value.text = cppexp.name_map[attr.value.hash];
		}

		// Count how many bytes are needed to store all attribute text
		int text_size = 0;
		for (int i = 0; i < cppexp.db->text_attributes.size(); i++)
		{
			clcpp::TextAttribute& attr = cppexp.db->text_attributes[i];
			text_size += strlen(attr.value) + 1;
		}

		// Allocate memory for them
		cppexp.db->text_attribute_data = cppexp.allocator.Alloc<char>(text_size);

		// Copy all text attribute data to the main store and reassign pointers
		char* pos = (char*)cppexp.db->text_attribute_data;
		for (int i = 0; i < cppexp.db->text_attributes.size(); i++)
		{
			clcpp::TextAttribute& attr = cppexp.db->text_attributes[i];
			strcpy(pos, attr.value);
			attr.value = pos;
			pos += strlen(attr.value) + 1;
		}
	}


	template <typename PARENT_TYPE, typename FIELD_TYPE, typename CHILD_TYPE>
	void Link(clcpp::CArray<PARENT_TYPE>& parents, const FIELD_TYPE* (PARENT_TYPE::*field), clcpp::CArray<const CHILD_TYPE*>& children)
	{
		// Create a lookup table from hash ID to child
		typedef std::multimap<unsigned int, const CHILD_TYPE*> ChildMap;
		typedef std::pair<ChildMap::iterator, ChildMap::iterator> ChildMapRange;
		ChildMap child_map;
		for (int i = 0; i < children.size(); i++)
		{
			const CHILD_TYPE* child = children[i];
			child_map.insert(ChildMap::value_type(child->name.hash, child));
		}

		// Link up the pointers
		for (int i = 0; i < parents.size(); i++)
		{
			PARENT_TYPE& parent = parents[i];
			unsigned int hash_id = (unsigned int)(parent.*field);
			ChildMap::iterator j = child_map.find(hash_id);
			if (j != child_map.end())
			{
				(parent.*field) = (FIELD_TYPE*)j->second;
			}
		}
	}


	// Link for array sources
	template <typename PARENT_TYPE, typename FIELD_TYPE, typename CHILD_TYPE, int N>
	void Link(clcpp::CArray<PARENT_TYPE>& parents, const FIELD_TYPE* (PARENT_TYPE::*field)[N], clcpp::CArray<const CHILD_TYPE*>& children)
	{
		// Create a lookup table from hash ID to child
		typedef std::multimap<unsigned int, const CHILD_TYPE*> ChildMap;
		typedef std::pair<ChildMap::iterator, ChildMap::iterator> ChildMapRange;
		ChildMap child_map;
		for (int i = 0; i < children.size(); i++)
		{
			const CHILD_TYPE* child = children[i];
			child_map.insert(ChildMap::value_type(child->name.hash, child));
		}

		// Link up the pointers
		for (int i = 0; i < parents.size(); i++)
		{
			PARENT_TYPE& parent = parents[i];
			for (int j = 0; j < N; j++)
			{
				unsigned int hash_id = (unsigned int)(parent.*field)[j];
				ChildMap::iterator k = child_map.find(hash_id);
				if (k != child_map.end())
				{
					(parent.*field)[j] = (FIELD_TYPE*)k->second;
				}
			}
		}
	}


	int ReturnParameterIndex(const clcpp::CArray<const clcpp::Field*>& parameters)
	{
		// Linear search for the named return value
		static unsigned int return_hash = clcpp::internal::HashNameString("return");
		for (int i = 0; i < parameters.size(); i++)
		{
			if (parameters[i]->name.hash == return_hash)
			{
				return i;
			}
		}
		return -1;
	}


	void AssignReturnParameters(CppExport& cppexp)
	{
		// Iterate over every function that has the return parameter in its parameter list
		for (int i = 0; i < cppexp.db->functions.size(); i++)
		{
			clcpp::Function& func = cppexp.db->functions[i];
			int return_index = ReturnParameterIndex(func.parameters);
			if (return_index == -1)
			{
				continue;
			}

			// Assign the return parameter and remove it from the parameter list
			func.return_parameter = func.parameters[return_index];
			func.parameters.unstable_remove(return_index);
		}
	}


	template <typename TYPE>
	int CountGlobalPrimitives(const clcpp::CArray<TYPE>& primitives)
	{
		// Finding all unparented primitives
		int nb_global_primitives = 0;
		for (int i = 0; i < primitives.size(); i++)
		{
			if (primitives[i].parent == 0)
			{
				nb_global_primitives++;
			}
		}
		return nb_global_primitives;
	}


	template <typename TYPE>
	void GatherGlobalPrimitives(clcpp::CArray<const TYPE*>& dest, const clcpp::CArray<TYPE>& src, StackAllocator& allocator)
	{
		// Allocate enough space for the primitives
		int nb_global_primitives = CountGlobalPrimitives(src);
		dest.copy(clcpp::CArray<const TYPE*>(allocator.Alloc<const TYPE*>(nb_global_primitives), nb_global_primitives));

		// Gather all unparented primitives
		int index = 0;
		for (int i = 0; i < src.size(); i++)
		{
			if (src[i].parent == 0)
			{
				dest[index++] = &src[i];
			}
		}
	}


	void BuildGlobalNamespace(CppExport& cppexp)
	{
		GatherGlobalPrimitives(cppexp.db->global_namespace.namespaces, cppexp.db->namespaces, cppexp.allocator);
		GatherGlobalPrimitives(cppexp.db->global_namespace.types, cppexp.db->types, cppexp.allocator);
		GatherGlobalPrimitives(cppexp.db->global_namespace.enums, cppexp.db->enums, cppexp.allocator);
		GatherGlobalPrimitives(cppexp.db->global_namespace.classes, cppexp.db->classes, cppexp.allocator);
		GatherGlobalPrimitives(cppexp.db->global_namespace.functions, cppexp.db->functions, cppexp.allocator);
		GatherGlobalPrimitives(cppexp.db->global_namespace.templates, cppexp.db->templates, cppexp.allocator);
	}


	void GatherTypePrimitives(CppExport& cppexp)
	{
		// Allocate the array
		int nb_type_primitives = cppexp.db->types.size() + cppexp.db->classes.size() + cppexp.db->enums.size() + cppexp.db->template_types.size();
		cppexp.db->type_primitives.copy(clcpp::CArray<const clcpp::Type*>(cppexp.allocator.Alloc<const clcpp::Type*>(nb_type_primitives), nb_type_primitives));

		// Generate references to anything that is a type
		int index = 0;
		for (int i = 0; i < cppexp.db->types.size(); i++)
		{
			cppexp.db->type_primitives[index++] = &cppexp.db->types[i];
		}
		for (int i = 0; i < cppexp.db->classes.size(); i++)
		{
			cppexp.db->type_primitives[index++] = &cppexp.db->classes[i];
		}
		for (int i = 0; i < cppexp.db->enums.size(); i++)
		{
			cppexp.db->type_primitives[index++] = &cppexp.db->enums[i];
		}
		for (int i = 0; i < cppexp.db->template_types.size(); i++)
		{
			cppexp.db->type_primitives[index++] = &cppexp.db->template_types[i];
		}
	}


	// Sort an array of primitive pointers by name
	bool SortPrimitiveByName(const clcpp::Primitive* a, const clcpp::Primitive* b)
	{
		return a->name.hash < b->name.hash;
	}
	template <typename TYPE>
	void SortPrimitives(clcpp::CArray<const TYPE*>& primitives)
	{
		std::sort(primitives.data(), primitives.data() + primitives.size(), SortPrimitiveByName);
	}


	// Overloads for sorting primitive arrays within a primitive
	void SortPrimitives(clcpp::Enum& primitive)
	{
		SortPrimitives(primitive.constants);
		SortPrimitives(primitive.attributes);
	}
	void SortPrimitives(clcpp::Field& primitive)
	{
		SortPrimitives(primitive.attributes);
	}
	void SortPrimitives(clcpp::Function& primitive)
	{
		SortPrimitives(primitive.parameters);
		SortPrimitives(primitive.attributes);
	}
	void SortPrimitives(clcpp::Class& primitive)
	{
		SortPrimitives(primitive.enums);
		SortPrimitives(primitive.classes);
		SortPrimitives(primitive.methods);
		SortPrimitives(primitive.fields);
		SortPrimitives(primitive.attributes);
		SortPrimitives(primitive.templates);
	}
	void SortPrimitives(clcpp::Template& primitive)
	{
		SortPrimitives(primitive.instances);
	}
	void SortPrimitives(clcpp::Namespace& primitive)
	{
		SortPrimitives(primitive.namespaces);
		SortPrimitives(primitive.types);
		SortPrimitives(primitive.enums);
		SortPrimitives(primitive.classes);
		SortPrimitives(primitive.functions);
		SortPrimitives(primitive.templates);
	}


	// Iterate over all provided primitives and sort their primitive arrays
	template <typename TYPE>
	void SortPrimitives(clcpp::CArray<TYPE>& primitives)
	{
		for (int i = 0; i < primitives.size(); i++)
		{
			SortPrimitives(primitives[i]);
		}
	}


	void FindClassConstructors(CppExport& cppexp)
	{
		// Search each class method list for constructors and destructors
		clcpp::CArray<clcpp::Class>& classes = cppexp.db->classes;
		for (int i = 0; i < classes.size(); i++)
		{
			clcpp::Class& cls = classes[i];

			// Methods in a class have fully-scoped names so these need to be constructed first
			// TODO: This isn't ideal for the client :/
			std::string construct_name = std::string(cls.name.text) + "::ConstructObject";
			std::string destruct_name = std::string(cls.name.text) + "::DestructObject";
			unsigned int construct_hash = clcpp::internal::HashNameString(construct_name.c_str());
			unsigned int destruct_hash = clcpp::internal::HashNameString(destruct_name.c_str());

			cls.constructor = clcpp::FindPrimitive(cls.methods, construct_hash);
			cls.destructor = clcpp::FindPrimitive(cls.methods, destruct_hash);
		}
	}


	template <typename TYPE>
	bool VerifyPtr(CppExport& cppexp, const TYPE* ptr)
	{
		// Cast to a hash value
		unsigned int hash = (unsigned int)ptr;

		// Report an error if the reference hasn't been patched up
		CppExport::NameMap::const_iterator i = cppexp.name_map.find(hash);
		if (i != cppexp.name_map.end())
		{
			LOG(main, ERROR, "Couldn't find reference to '%s'\n", i->second);
			return false;
		}

		return true;
	}

	// Overloads for verifying the pointers of each primitive type
	bool VerifyPrimitive(CppExport& cppexp, const clcpp::Primitive& primitive)
	{
		return VerifyPtr(cppexp, primitive.parent);
	}
	bool VerifyPrimitive(CppExport& cppexp, const clcpp::Field& primitive)
	{
		return VerifyPrimitive(cppexp, (clcpp::Primitive&)primitive) & VerifyPtr(cppexp, primitive.type);
	}
	bool VerifyPrimitive(CppExport& cppexp, const clcpp::Function& primitive)
	{
		return VerifyPrimitive(cppexp, (clcpp::Primitive&)primitive) & VerifyPtr(cppexp, primitive.return_parameter);
	}
	bool VerifyPrimitive(CppExport& cppexp, const clcpp::TemplateType& primitive)
	{
		bool result = VerifyPrimitive(cppexp, (clcpp::Primitive&)primitive);
		for (int i = 0; i < clcpp::TemplateType::MAX_NB_ARGS; i++)
		{
			result &= VerifyPtr(cppexp, primitive.parameter_types[i]);
		}
		return result;
	}
	bool VerifyPrimitive(CppExport& cppexp, const clcpp::Class& primitive)
	{
		return VerifyPrimitive(cppexp, (clcpp::Type&)primitive) & VerifyPtr(cppexp, primitive.base_class);
	}

	template <typename TYPE>
	bool VerifyPrimitives(CppExport& cppexp, const clcpp::CArray<TYPE>& primitives)
	{
		// Verifies all primitives in an array
		bool result = true;
		for (int i = 0; i < primitives.size(); i++)
		{
			result &= VerifyPrimitive(cppexp, primitives[i]);
		}
		return result;
	}

	bool VerifyPrimitives(CppExport& cppexp)
	{
		bool result = true;
		result &= VerifyPrimitives(cppexp, cppexp.db->types);
		result &= VerifyPrimitives(cppexp, cppexp.db->enum_constants);
		result &= VerifyPrimitives(cppexp, cppexp.db->enums);
		result &= VerifyPrimitives(cppexp, cppexp.db->fields);
		result &= VerifyPrimitives(cppexp, cppexp.db->functions);
		result &= VerifyPrimitives(cppexp, cppexp.db->classes);
		result &= VerifyPrimitives(cppexp, cppexp.db->templates);
		result &= VerifyPrimitives(cppexp, cppexp.db->template_types);
		result &= VerifyPrimitives(cppexp, cppexp.db->namespaces);
		result &= VerifyPrimitives(cppexp, cppexp.db->flag_attributes);
		result &= VerifyPrimitives(cppexp, cppexp.db->int_attributes);
		result &= VerifyPrimitives(cppexp, cppexp.db->float_attributes);
		result &= VerifyPrimitives(cppexp, cppexp.db->name_attributes);
		result &= VerifyPrimitives(cppexp, cppexp.db->text_attributes);
		return result;
	}
}


bool BuildCppExport(const cldb::Database& db, CppExport& cppexp)
{
	// Allocate the in-memory database
	cppexp.db = cppexp.allocator.Alloc<clcpp::internal::DatabaseMem>(1);

	// Build all the name data ready for the client to use and the exporter to debug with
	BuildNames(db, cppexp);

	// Generate a raw clcpp equivalent of the cldb database. At this point no primitives
	// will physically point to or contain each other, but they will reference each other
	// using hash values aliased in their pointers.
	BuildCArray<cldb::Type>(cppexp, cppexp.db->types, db);
	BuildCArray<cldb::EnumConstant>(cppexp, cppexp.db->enum_constants, db);
	BuildCArray<cldb::Enum>(cppexp, cppexp.db->enums, db);
	BuildCArray<cldb::Field>(cppexp, cppexp.db->fields, db);
	BuildCArray<cldb::Function>(cppexp, cppexp.db->functions, db);
	BuildCArray<cldb::Class>(cppexp, cppexp.db->classes, db);
	BuildCArray<cldb::Template>(cppexp, cppexp.db->templates, db);
	BuildCArray<cldb::TemplateType>(cppexp, cppexp.db->template_types, db);
	BuildCArray<cldb::Namespace>(cppexp, cppexp.db->namespaces, db);
	BuildCArray<cldb::FlagAttribute>(cppexp, cppexp.db->flag_attributes, db);
	BuildCArray<cldb::IntAttribute>(cppexp, cppexp.db->int_attributes, db);
	BuildCArray<cldb::FloatAttribute>(cppexp, cppexp.db->float_attributes, db);
	BuildCArray<cldb::NameAttribute>(cppexp, cppexp.db->name_attributes, db);
	BuildCArray<cldb::TextAttribute>(cppexp, cppexp.db->text_attributes, db);

	// Now ensure all name and text data are pointing into the data to be memory mapped
	AssignAttributeNamesAndText(cppexp);

	// Generate a list of references to all type primitives so that runtime serialisation code
	// can quickly look them up.
	GatherTypePrimitives(cppexp);

	// Construct the primitive scope hierarchy, pointing primitives at their parents
	// and adding them to the arrays within their parents.
	// TODO: Pull multimap construction out of the functions so that they're not repeatedly generated
	Parent(cppexp.db->enums, &clcpp::Enum::constants, cppexp.db->enum_constants, cppexp.allocator);
	Parent(cppexp.db->functions, &clcpp::Function::parameters, cppexp.db->fields, cppexp.allocator);
	Parent(cppexp.db->classes, &clcpp::Class::enums, cppexp.db->enums, cppexp.allocator);
	Parent(cppexp.db->classes, &clcpp::Class::classes, cppexp.db->classes, cppexp.allocator);
	Parent(cppexp.db->classes, &clcpp::Class::methods, cppexp.db->functions, cppexp.allocator);
	Parent(cppexp.db->classes, &clcpp::Class::fields, cppexp.db->fields, cppexp.allocator);
	Parent(cppexp.db->classes, &clcpp::Class::templates, cppexp.db->templates, cppexp.allocator);
	Parent(cppexp.db->namespaces, &clcpp::Namespace::namespaces, cppexp.db->namespaces, cppexp.allocator);
	Parent(cppexp.db->namespaces, &clcpp::Namespace::types, cppexp.db->types, cppexp.allocator);
	Parent(cppexp.db->namespaces, &clcpp::Namespace::enums, cppexp.db->enums, cppexp.allocator);
	Parent(cppexp.db->namespaces, &clcpp::Namespace::classes, cppexp.db->classes, cppexp.allocator);
	Parent(cppexp.db->namespaces, &clcpp::Namespace::functions, cppexp.db->functions, cppexp.allocator);
	Parent(cppexp.db->namespaces, &clcpp::Namespace::templates, cppexp.db->templates, cppexp.allocator);
	Parent(cppexp.db->templates, &clcpp::Template::instances, cppexp.db->template_types, cppexp.allocator);

	// Construct the primitive hierarchy for attributes by first collecting all attributes into
	// a single pointer array
	clcpp::CArray<clcpp::Attribute*> attributes;
	BuildAttributePtrArray(cppexp, attributes);
	Parent(cppexp.db->enums, &clcpp::Enum::attributes, attributes, cppexp.allocator);
	Parent(cppexp.db->fields, &clcpp::Field::attributes, attributes, cppexp.allocator);
	Parent(cppexp.db->functions, &clcpp::Function::attributes, attributes, cppexp.allocator);
	Parent(cppexp.db->classes, &clcpp::Class::attributes, attributes, cppexp.allocator);

	// Link up any references between primitives
	Link(cppexp.db->fields, &clcpp::Field::type, cppexp.db->type_primitives);
	Link(cppexp.db->classes, &clcpp::Class::base_class, cppexp.db->type_primitives);
	Link(cppexp.db->template_types, &clcpp::TemplateType::parameter_types, cppexp.db->type_primitives);

	// Return parameters are parented to their functions as parameters. Move them from
	// wherever they are in the list and into the return parameter data member.
	AssignReturnParameters(cppexp);

	// Gather any unparented primitives into the root namespace
	BuildGlobalNamespace(cppexp);

	// Sort any primitive pointer arrays in the database by name hash, ascending. This
	// is to allow fast O(logN) searching of the primitive arrays at runtime with a
	// binary search.
	SortPrimitives(cppexp.db->enums);
	SortPrimitives(cppexp.db->fields);
	SortPrimitives(cppexp.db->functions);
	SortPrimitives(cppexp.db->classes);
	SortPrimitives(cppexp.db->templates);
	SortPrimitives(cppexp.db->namespaces);
	SortPrimitives(cppexp.db->type_primitives);

	// Each class may have constructor/destructor methods in their method list. Run through
	// each class and make pointers to these in the class. This is done after sorting so that
	// local searches can take advantage of clcpp::FindPrimitive.
	FindClassConstructors(cppexp);

	// Primitives reference each other via their names (hash codes). This code first of all copies
	// hashes into the pointers and then patches them up via lookup. If the input database doesn't
	// contain primitives that others reference then at this point, certain primitives will contain
	// effectively garbage pointers. Do a check here for that.
	return VerifyPrimitives(cppexp);
}


void SaveCppExport(CppExport& cppexp, const char* filename)
{
	PtrRelocator relocator(cppexp.allocator.GetData(), cppexp.allocator.GetAllocatedSize());

	// The position of the data member within a CArray is fixed, independent of type
	size_t array_data_offset = clcpp::CArray<int>::data_offset();

	// Construct schemas for all memory-mapped clcpp types

	PtrSchema& schema_database = relocator.AddSchema<clcpp::internal::DatabaseMem>()
		(&clcpp::internal::DatabaseMem::name_text_data)
		(&clcpp::internal::DatabaseMem::names, array_data_offset)
		(&clcpp::internal::DatabaseMem::types, array_data_offset)
		(&clcpp::internal::DatabaseMem::enum_constants, array_data_offset)
		(&clcpp::internal::DatabaseMem::enums, array_data_offset)
		(&clcpp::internal::DatabaseMem::fields, array_data_offset)
		(&clcpp::internal::DatabaseMem::functions, array_data_offset)
		(&clcpp::internal::DatabaseMem::classes, array_data_offset)
		(&clcpp::internal::DatabaseMem::template_types, array_data_offset)
		(&clcpp::internal::DatabaseMem::templates, array_data_offset)
		(&clcpp::internal::DatabaseMem::namespaces, array_data_offset)
		(&clcpp::internal::DatabaseMem::text_attribute_data)
		(&clcpp::internal::DatabaseMem::flag_attributes, array_data_offset)
		(&clcpp::internal::DatabaseMem::int_attributes, array_data_offset)
		(&clcpp::internal::DatabaseMem::float_attributes, array_data_offset)
		(&clcpp::internal::DatabaseMem::name_attributes, array_data_offset)
		(&clcpp::internal::DatabaseMem::text_attributes, array_data_offset)
		(&clcpp::internal::DatabaseMem::type_primitives, array_data_offset)
		(&clcpp::Namespace::namespaces, array_data_offset + offsetof(clcpp::internal::DatabaseMem, global_namespace))
		(&clcpp::Namespace::types, array_data_offset + offsetof(clcpp::internal::DatabaseMem, global_namespace))
		(&clcpp::Namespace::enums, array_data_offset + offsetof(clcpp::internal::DatabaseMem, global_namespace))
		(&clcpp::Namespace::classes, array_data_offset + offsetof(clcpp::internal::DatabaseMem, global_namespace))
		(&clcpp::Namespace::functions, array_data_offset + offsetof(clcpp::internal::DatabaseMem, global_namespace));

	PtrSchema& schema_name = relocator.AddSchema<clcpp::Name>()
		(&clcpp::Name::text);

	PtrSchema& schema_primitive = relocator.AddSchema<clcpp::Primitive>()
		(&clcpp::Name::text, offsetof(clcpp::Primitive, name))
		(&clcpp::Primitive::parent);

	PtrSchema& schema_type = relocator.AddSchema<clcpp::Type>(&schema_primitive);
	PtrSchema& schema_enum_constant = relocator.AddSchema<clcpp::EnumConstant>(&schema_primitive);

	PtrSchema& schema_enum = relocator.AddSchema<clcpp::Enum>(&schema_type)
		(&clcpp::Enum::constants, array_data_offset)
		(&clcpp::Enum::attributes, array_data_offset);

	PtrSchema& schema_field = relocator.AddSchema<clcpp::Field>(&schema_primitive)
		(&clcpp::Field::type)
		(&clcpp::Field::attributes, array_data_offset);

	PtrSchema& schema_function = relocator.AddSchema<clcpp::Function>(&schema_primitive)
		(&clcpp::Function::return_parameter)
		(&clcpp::Function::parameters, array_data_offset)
		(&clcpp::Function::attributes, array_data_offset);

	PtrSchema& schema_class = relocator.AddSchema<clcpp::Class>(&schema_type)
		(&clcpp::Class::base_class)
		(&clcpp::Class::constructor)
		(&clcpp::Class::destructor)
		(&clcpp::Class::enums, array_data_offset)
		(&clcpp::Class::classes, array_data_offset)
		(&clcpp::Class::methods, array_data_offset)
		(&clcpp::Class::fields, array_data_offset)
		(&clcpp::Class::attributes, array_data_offset)
		(&clcpp::Class::templates, array_data_offset);

	PtrSchema& schema_template_type = relocator.AddSchema<clcpp::TemplateType>(&schema_type)
		(&clcpp::TemplateType::parameter_types, 0)
		(&clcpp::TemplateType::parameter_types, sizeof(void*))
		(&clcpp::TemplateType::parameter_types, sizeof(void*) * 2)
		(&clcpp::TemplateType::parameter_types, sizeof(void*) * 3);

	PtrSchema& schema_template = relocator.AddSchema<clcpp::Template>(&schema_primitive)
		(&clcpp::Template::instances, array_data_offset);

	PtrSchema& schema_namespace = relocator.AddSchema<clcpp::Namespace>(&schema_primitive)
		(&clcpp::Namespace::namespaces, array_data_offset)
		(&clcpp::Namespace::types, array_data_offset)
		(&clcpp::Namespace::enums, array_data_offset)
		(&clcpp::Namespace::classes, array_data_offset)
		(&clcpp::Namespace::functions, array_data_offset)
		(&clcpp::Namespace::templates, array_data_offset);

	PtrSchema& schema_int_attribute = relocator.AddSchema<clcpp::IntAttribute>(&schema_primitive);
	PtrSchema& schema_float_attribute = relocator.AddSchema<clcpp::FloatAttribute>(&schema_primitive);

	PtrSchema& schema_name_attribute = relocator.AddSchema<clcpp::NameAttribute>(&schema_primitive)
		(&clcpp::Name::text, offsetof(clcpp::NameAttribute, value));

	PtrSchema& schema_text_attribute = relocator.AddSchema<clcpp::TextAttribute>(&schema_primitive)
		(&clcpp::TextAttribute::value);

	PtrSchema& schema_ptr = relocator.AddSchema<void*>()(0);

	// Add pointers from the base database object
	relocator.AddPointers(schema_database, cppexp.db);
	relocator.AddPointers(schema_name, cppexp.db->names);
	relocator.AddPointers(schema_type, cppexp.db->types);
	relocator.AddPointers(schema_enum_constant, cppexp.db->enum_constants);
	relocator.AddPointers(schema_enum, cppexp.db->enums);
	relocator.AddPointers(schema_field, cppexp.db->fields);
	relocator.AddPointers(schema_function, cppexp.db->functions);
	relocator.AddPointers(schema_class, cppexp.db->classes);
	relocator.AddPointers(schema_template_type, cppexp.db->template_types);
	relocator.AddPointers(schema_template, cppexp.db->templates);
	relocator.AddPointers(schema_namespace, cppexp.db->namespaces);
	relocator.AddPointers(schema_primitive, cppexp.db->flag_attributes);
	relocator.AddPointers(schema_int_attribute, cppexp.db->int_attributes);
	relocator.AddPointers(schema_float_attribute, cppexp.db->float_attributes);
	relocator.AddPointers(schema_name_attribute, cppexp.db->name_attributes);
	relocator.AddPointers(schema_text_attribute, cppexp.db->text_attributes);
	relocator.AddPointers(schema_ptr, cppexp.db->type_primitives);

	// Add pointers for the array objects within each primitive
	// Note that currently these are expressed as general pointer relocation instructions
	// with a specific "pointer" schema. This is 12 bytes per AddPointers call (which gets
	// into the hundreds/thousands) that could be trimmed a little if a specific pointer
	// relocation instruction was introduced that would cost 8 bytes.
	for (int i = 0; i < cppexp.db->enums.size(); i++)
	{
		relocator.AddPointers(schema_ptr, cppexp.db->enums[i].constants);
		relocator.AddPointers(schema_ptr, cppexp.db->enums[i].attributes);
	}
	for (int i = 0; i < cppexp.db->fields.size(); i++)
	{
		relocator.AddPointers(schema_ptr, cppexp.db->fields[i].attributes);
	}
	for (int i = 0; i < cppexp.db->functions.size(); i++)
	{
		relocator.AddPointers(schema_ptr, cppexp.db->functions[i].parameters);
		relocator.AddPointers(schema_ptr, cppexp.db->functions[i].attributes);
	}
	for (int i = 0; i < cppexp.db->classes.size(); i++)
	{
		clcpp::Class& cls = cppexp.db->classes[i];
		relocator.AddPointers(schema_ptr, cls.enums);
		relocator.AddPointers(schema_ptr, cls.classes);
		relocator.AddPointers(schema_ptr, cls.methods);
		relocator.AddPointers(schema_ptr, cls.fields);
		relocator.AddPointers(schema_ptr, cls.attributes);
		relocator.AddPointers(schema_ptr, cls.templates);
	}
	for (int i = 0; i < cppexp.db->templates.size(); i++)
	{
		relocator.AddPointers(schema_ptr, cppexp.db->templates[i].instances);
	}
	for (int i = 0; i < cppexp.db->namespaces.size(); i++)
	{
		relocator.AddPointers(schema_ptr, cppexp.db->namespaces[i].namespaces);
		relocator.AddPointers(schema_ptr, cppexp.db->namespaces[i].types);
		relocator.AddPointers(schema_ptr, cppexp.db->namespaces[i].enums);
		relocator.AddPointers(schema_ptr, cppexp.db->namespaces[i].classes);
		relocator.AddPointers(schema_ptr, cppexp.db->namespaces[i].functions);
		relocator.AddPointers(schema_ptr, cppexp.db->namespaces[i].templates);
	}

	// Make all pointers relative to the start address
	relocator.MakeRelative();

	// Open the output file
	FILE* fp = fopen(filename, "wb");
	if (fp == 0)
	{
		return;
	}

	// Count the total number of pointer offsets
	size_t nb_ptr_offsets = 0;
	const std::vector<PtrSchema*>& schemas = relocator.GetSchemas();
	for (size_t i = 0; i < schemas.size(); i++)
	{
		nb_ptr_offsets += schemas[i]->ptr_offsets.size();
	}

	// Write the header
	clcpp::internal::DatabaseFileHeader header;
	header.nb_ptr_schemas = schemas.size();
	header.nb_ptr_offsets = nb_ptr_offsets;
	const std::vector<PtrRelocation>& relocations = relocator.GetRelocations();
	header.nb_ptr_relocations = relocations.size();
	header.data_size = cppexp.allocator.GetAllocatedSize();
	fwrite(&header, sizeof(header), 1, fp);

	// Write the complete memory map
	fwrite(cppexp.allocator.GetData(), cppexp.allocator.GetAllocatedSize(), 1, fp);

	// Write the stride of each schema and the location of their pointers
	size_t ptrs_offset = 0;
	for (size_t i = 0; i < schemas.size(); i++)
	{
		const PtrSchema& s = *schemas[i];
		size_t nb_ptrs = s.ptr_offsets.size();
		fwrite(&s.stride, sizeof(s.stride), 1, fp);
		fwrite(&ptrs_offset, sizeof(ptrs_offset), 1, fp);
		fwrite(&nb_ptrs, sizeof(nb_ptrs), 1, fp);
		ptrs_offset += nb_ptrs;
	}

	// Write the schema pointer offsets
	for (size_t i = 0; i < schemas.size(); i++)
	{
		const PtrSchema& s = *schemas[i];
		fwrite(&s.ptr_offsets.front(), sizeof(size_t), s.ptr_offsets.size(), fp);
	}

	// Write the relocations
	fwrite(&relocations.front(), sizeof(PtrRelocation), relocations.size(), fp);

	fclose(fp);
}


namespace
{
	bool SortFieldByOffset(const clcpp::Field* a, const clcpp::Field* b)
	{
		return a->offset < b->offset;
	}
	bool SortEnumConstantByValue(const clcpp::EnumConstant* a, const clcpp::EnumConstant* b)
	{
		return a->value < b->value;
	}


	template <typename TYPE>
	void LogPrimitives(const clcpp::CArray<const TYPE*>& primitives)
	{
		for (int i = 0; i < primitives.size(); i++)
		{
			LogPrimitive(*primitives[i]);
			LOG_NEWLINE(cppexp);
		}
	}


	void LogField(const clcpp::Field& field, bool name = true)
	{
		LOG_APPEND(cppexp, INFO, "%s", field.is_const ? "const " : "");
		LOG_APPEND(cppexp, INFO, "%s", field.type->name.text);
		LOG_APPEND(cppexp, INFO, "%s", field.modifier == clcpp::Field::MODIFIER_POINTER ? "*" :
			field.modifier == clcpp::Field::MODIFIER_REFERENCE ? "&" : "");

		if (name)
		{
			LOG_APPEND(cppexp, INFO, " %s", field.name.text);
		}
	}


	void LogPrimitive(const clcpp::Field& field)
	{
		LOG(cppexp, INFO, "");
		LogField(field);
		LOG_APPEND(cppexp, INFO, ";");
	}


	void LogPrimitive(const clcpp::Function& func)
	{
		if (func.return_parameter)
		{
			LOG(cppexp, INFO, "");
			LogField(*func.return_parameter, false);
		}
		else
		{
			LOG(cppexp, INFO, "void");
		}

		LOG_APPEND(cppexp, INFO, " %s(", func.name.text);

		// Sort parameters by index for viewing
		clcpp::CArray<const clcpp::Field*> sorted_parameters = func.parameters;
		std::sort(sorted_parameters.data(), sorted_parameters.data() + sorted_parameters.size(), SortFieldByOffset);

		for (int i = 0; i < sorted_parameters.size(); i++)
		{
			LogField(*sorted_parameters[i]);
			if (i != sorted_parameters.size() - 1)
			{
				LOG_APPEND(cppexp, INFO, ", ");
			}
		}

		LOG_APPEND(cppexp, INFO, ");");
	}


	void LogPrimitive(const clcpp::EnumConstant& constant)
	{
		LOG(cppexp, INFO, "%s = %d,", constant.name.text, constant.value);
	}


	void LogPrimitive(const clcpp::Enum& e)
	{
		LOG(cppexp, INFO, "enum %s\n", e.name.text);
		LOG(cppexp, INFO, "{\n");
		LOG_PUSH_INDENT(cppexp);

		// Sort constants by value for viewing
		clcpp::CArray<const clcpp::EnumConstant*> sorted_constants = e.constants;
		std::sort(sorted_constants.data(), sorted_constants.data() + sorted_constants.size(), SortEnumConstantByValue);

		LogPrimitives(sorted_constants);

		LOG_POP_INDENT(cppexp);
		LOG(cppexp, INFO, "};");
	}


	void LogPrimitive(const clcpp::TemplateType& tt)
	{
		LOG(cppexp, INFO, "%s\n", tt.name.text);
	}


	void LogPrimitive(const clcpp::Template& t)
	{
		const char* name = t.name.text;
		LOG(cppexp, INFO, "template %s\n", name);
		LOG(cppexp, INFO, "{\n");
		LOG_PUSH_INDENT(cppexp);

		LogPrimitives(t.instances);

		LOG_POP_INDENT(cppexp);
		LOG(cppexp, INFO, "};");
	}

	
	void LogPrimitive(const clcpp::Class& cls)
	{
		LOG(cppexp, INFO, "class %s", cls.name.text);
		if (cls.base_class)
		{
			LOG_APPEND(cppexp, INFO, " : public %s\n", cls.base_class->name.text);
		}
		else
		{
			LOG_APPEND(cppexp, INFO, "\n");
		}
		LOG(cppexp, INFO, "{\n");
		LOG_PUSH_INDENT(cppexp);

		// Sort fields by offset for viewing
		clcpp::CArray<const clcpp::Field*> sorted_fields = cls.fields;
		std::sort(sorted_fields.data(), sorted_fields.data() + sorted_fields.size(), SortFieldByOffset);

		LogPrimitives(cls.classes);
		LogPrimitives(sorted_fields);
		LogPrimitives(cls.enums);
		LogPrimitives(cls.methods);
		LogPrimitives(cls.templates);

		LOG_POP_INDENT(cppexp);
		LOG(cppexp, INFO, "};");
	}


	void LogPrimitive(const clcpp::Namespace& ns)
	{
		if (ns.name.text)
		{
			LOG(cppexp, INFO, "namespace %s\n", ns.name.text);
			LOG(cppexp, INFO, "{\n");
			LOG_PUSH_INDENT(cppexp);
		}

		LogPrimitives(ns.namespaces);
		LogPrimitives(ns.classes);
		LogPrimitives(ns.enums);
		LogPrimitives(ns.functions);
		LogPrimitives(ns.templates);

		if (ns.name.text)
		{
			LOG_POP_INDENT(cppexp);
			LOG(cppexp, INFO, "}");
		}
	}
}


void WriteCppExportAsText(const CppExport& cppexp, const char* filename)
{
	LOG_TO_FILE(cppexp, ALL, filename);
	LogPrimitive(cppexp.db->global_namespace);
}