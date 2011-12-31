/*
A distributed cartesian cell-refinable grid

Copyright 2009, 2010, 2011 Finnish Meteorological Institute

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License version 3
as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#ifndef DCCRG_HPP
#define DCCRG_HPP

/*!
\mainpage Distributed Cartesian Cell-Refinable Grid.

\section intro_sec Introduction
dccrg is a grid library for simulations using the finite volume method.
See the examples directory for some simple examples and the tests directory
for more advanced usage of dccrg.
*/



/*
If the size of the data in every cell is known in advance by the user, neighbour data updates can be optimized by defining DCCRG_CELL_DATA_SIZE_FROM_USER, in which case:
	-UserData class must have a static function size() which returns the size of data in bytes of all cells.
	-UserData instances must have a function at() which returns the starting address of their data.

Additionally if DCCRG_USER_MPI_DATA_TYPE is defined:
	-UserData class must have a static function mpi_datatype() which returns the MPI_Datatype of all cells.
	-UserData function size() is not needed (size() == 1 is assumed).
	-Cells can have non-contiguous data.

If DCCRG_SEND_SINGLE_CELLS is defined then cell data is sent one cell at a time.

*/
#ifdef DCCRG_USER_MPI_DATA_TYPE
	#ifndef DCCRG_CELL_DATA_SIZE_FROM_USER
		#error DCCRG_CELL_DATA_SIZE_FROM_USER must defined when using DCCRG_USER_MPI_DATA_TYPE
	#endif
#endif

#include "algorithm"
#include "boost/array.hpp"
#include "boost/foreach.hpp"
#include "boost/mpi.hpp"
#include "boost/unordered_map.hpp"
#include "boost/unordered_set.hpp"
#include "cassert"
#include "cstdio"
#include "cstdlib"
#include "cstring"
#include "fstream"
#include "functional"
#include "limits"
#include "stdint.h"
#include "utility"
#include "vector"
#include "zoltan.h"

#ifdef USE_SFC
#include "sfc++.hpp"
#endif

#include "dccrg_index.hpp"
#include "dccrg_types.hpp"
#include "dccrg_constant_geometry.hpp"


namespace dccrg
{

template <class UserData, class UserGeometry = ConstantGeometry> class Dccrg : public UserGeometry
{

public:

	typedef typename std::pair<uint64_t, UserData> cell_and_data_pair_t;


	/*!
	Creates an uninitialized instance of the grid.

	The instance's set_geometry and initialize functions must be called
	before doing anything else, otherwise the results will be undefined.
	*/
	Dccrg()
	{
		this->initialized = false;
	}


	/*!
	\brief Initializes the instance of the grid with given parameters.

	The geometry of the grid instance must have been set using set_geometry
	before calling this function.

	Zoltan_Initialize must have been called before calling this function.

	comm: the grid will span all the processes in the communicator comm

	load_balancing_method:
		The method that Zoltan will use for load balancing, given as a string.
		All methods except REFTREE are supported, see this page for a list of available methods:
		http://www.cs.sandia.gov/Zoltan/ug_html/ug_alg.html#LB_METHOD

	neighbourhood_size:
		Determines which cells are considered neighbours.
		When calculating the neighbours of a given cell a cube of length
		2 * neighbourhood_size + 1 in every direction is considered, centered
		at the cell for which neighbours are being calculated.
		The unit lenght of the cube is the cell for which neighbours are being calculated.
		If neighbourhood_size == 0, only cells (or children within the volume of
		cells of the same size as the current cell) that share a face are considered.

	maximum_refinement_level:
		The maximum number of times an unrefined cell can be refined
		(replacingit with 8 smaller cells).
		If not given the maximum refinement level is maximized based on the grids initial size.

	periodic_in_x, y and z:
		The grid neighborhoods wrap around in periodic directions, e.g. if periodic in some
		direction cells on the opposite sides of the grid in that direction can be neighbors.
	*/
	void initialize(
		boost::mpi::communicator comm,
		const char* load_balancing_method,
		const unsigned int neighbourhood_size,
		const int maximum_refinement_level = -1,
		const bool periodic_in_x = false,
		const bool periodic_in_y = false,
		const bool periodic_in_z = false,
		const uint64_t sfc_caching_batches = 1
	)
	{
		if (this->initialized) {
			std::cerr << "Initialize function called for an already initialized dccrg" << std::endl;
			// TODO: throw an exception instead
			abort();
		}

		if (sfc_caching_batches == 0) {
			std::cerr << "sfc_caching_batches must be > 0" << std::endl;
			abort();
		}

		this->comm = comm;

		/*
		Setup Zoltan
		*/
		this->zoltan = Zoltan_Create(this->comm);
		if (this->zoltan == NULL) {
			std::cerr << "Zoltan_Create failed"  << std::endl;
			// TODO: throw an exception instead
			exit(EXIT_FAILURE);
		}

		// check whether Zoltan_LB_Partition is expected to fail
		if (strncmp(load_balancing_method, "NONE", sizeof("NONE")) == 0) {
			this->no_load_balancing = true;
		} else {
			this->no_load_balancing = false;
		}

		// reserved options that the user cannot change
		this->reserved_options.insert("EDGE_WEIGHT_DIM");
		this->reserved_options.insert("NUM_GID_ENTRIES");
		this->reserved_options.insert("OBJ_WEIGHT_DIM");
		this->reserved_options.insert("RETURN_LISTS");
		this->reserved_options.insert("NUM_GLOBAL_PARTS");
		this->reserved_options.insert("NUM_LOCAL_PARTS");
		this->reserved_options.insert("AUTO_MIGRATE");

		// set reserved options
		Zoltan_Set_Param(this->zoltan, "EDGE_WEIGHT_DIM", "0");	// 0 because Zoltan crashes in hierarchial with larger values
		Zoltan_Set_Param(this->zoltan, "NUM_GID_ENTRIES", "1");
		Zoltan_Set_Param(this->zoltan, "OBJ_WEIGHT_DIM", "1");
		Zoltan_Set_Param(this->zoltan, "RETURN_LISTS", "ALL");

		// set other options
		Zoltan_Set_Param(this->zoltan, "DEBUG_LEVEL", "0");
		Zoltan_Set_Param(this->zoltan, "HIER_DEBUG_LEVEL", "0");
		Zoltan_Set_Param(this->zoltan, "HIER_CHECKS", "0");
		Zoltan_Set_Param(this->zoltan, "LB_METHOD", load_balancing_method);
		Zoltan_Set_Param(this->zoltan, "REMAP", "1");


		// size of cells id in unsigned ints, but has to be 1 even when global id is uint64_t, for some reason
		/*char global_id_length_string[10];
		snprintf(global_id_length_string, 10, "%0i", int(sizeof(uint64_t) / sizeof(unsigned int)));*/

		// set the grids callback functions in Zoltan
		Zoltan_Set_Num_Obj_Fn(this->zoltan, &Dccrg<UserData, UserGeometry>::get_number_of_cells, this);
		Zoltan_Set_Obj_List_Fn(this->zoltan, &Dccrg<UserData, UserGeometry>::fill_cell_list, this);
		Zoltan_Set_Num_Geom_Fn(this->zoltan, &Dccrg<UserData, UserGeometry>::get_grid_dimensionality, NULL);
		Zoltan_Set_Geom_Multi_Fn(this->zoltan, &Dccrg<UserData, UserGeometry>::fill_with_cell_coordinates, this);
		Zoltan_Set_Num_Edges_Multi_Fn(this->zoltan, &Dccrg<UserData, UserGeometry>::fill_number_of_neighbours_for_cells, this);
		Zoltan_Set_Edge_List_Multi_Fn(this->zoltan, &Dccrg<UserData, UserGeometry>::fill_neighbour_lists, this);
		Zoltan_Set_HG_Size_CS_Fn(this->zoltan, &Dccrg<UserData, UserGeometry>::fill_number_of_hyperedges, this);
		Zoltan_Set_HG_CS_Fn(this->zoltan, &Dccrg<UserData, UserGeometry>::fill_hyperedge_lists, this);
		Zoltan_Set_HG_Size_Edge_Wts_Fn(this->zoltan, &Dccrg<UserData, UserGeometry>::fill_number_of_edge_weights, this);
		Zoltan_Set_HG_Edge_Wts_Fn(this->zoltan, &Dccrg<UserData, UserGeometry>::fill_edge_weights, this);
		Zoltan_Set_Hier_Num_Levels_Fn(this->zoltan, &Dccrg<UserData, UserGeometry>::get_number_of_load_balancing_hierarchies, this);
		Zoltan_Set_Hier_Part_Fn(this->zoltan, &Dccrg<UserData, UserGeometry>::get_part_number, this);
		Zoltan_Set_Hier_Method_Fn(this->zoltan, &Dccrg<UserData, UserGeometry>::set_partitioning_options, this);


		/*
		Set grid parameters
		*/

		this->periodic[0] = periodic_in_x;
		this->periodic[1] = periodic_in_y;
		this->periodic[2] = periodic_in_z;

		// set / check neighbourhood_of
		this->neighbourhood_size = neighbourhood_size;
		if (this->neighbourhood_size == 0) {
			{
			Types<3>::neighbourhood_item_t item = {0, 0, -1};
			this->neighbourhood_of.push_back(item);
			}
			{
			Types<3>::neighbourhood_item_t item = {0, -1, 0};
			this->neighbourhood_of.push_back(item);
			}
			{
			Types<3>::neighbourhood_item_t item = {-1, 0, 0};
			this->neighbourhood_of.push_back(item);
			}
			{
			Types<3>::neighbourhood_item_t item = {1, 0, 0};
			this->neighbourhood_of.push_back(item);
			}
			{
			Types<3>::neighbourhood_item_t item = {0, 1, 0};
			this->neighbourhood_of.push_back(item);
			}
			{
			Types<3>::neighbourhood_item_t item = {0, 0, 1};
			this->neighbourhood_of.push_back(item);
			}
		} else {
			for (int z = -neighbourhood_size; (unsigned int) abs(z) < neighbourhood_size + 1; z++)
			for (int y = -neighbourhood_size; (unsigned int) abs(y) < neighbourhood_size + 1; y++)
			for (int x = -neighbourhood_size; (unsigned int) abs(x) < neighbourhood_size + 1; x++) {
				if (x == 0 && y == 0 && z == 0) {
					continue;
				}
				const Types<3>::neighbourhood_item_t item = {x, y, z};
				this->neighbourhood_of.push_back(item);
			}
		}

		// set neighbourhood_to
		BOOST_FOREACH(Types<3>::neighbourhood_item_t offset, this->neighbourhood_of) {
			Types<3>::neighbourhood_item_t item = {-offset[0], -offset[1], -offset[2]};
			this->neighbourhood_to.push_back(item);
		}


		if (maximum_refinement_level < 0) {
			this->set_maximum_refinement_level(this->get_maximum_possible_refinement_level());
		} else if (!this->set_maximum_refinement_level(maximum_refinement_level)) {
			std::cerr << "Couldn't set maximum refinement level to " << maximum_refinement_level << std::endl;
			abort();
		}

		// TODO: check that the last index in the grid in every direction is less than error_index


		// create unrefined cells
		uint64_t cells_per_process = 0;
		if (this->grid_length < uint64_t(comm.size())) {
			cells_per_process = 1;
		} else if (this->grid_length % uint64_t(comm.size()) > 0) {
			cells_per_process = this->grid_length / uint64_t(comm.size()) + 1;
		} else {
			cells_per_process = this->grid_length / uint64_t(comm.size());
		}

		// some processes get fewer cells if grid size not divisible by comm.size()
		uint64_t procs_with_fewer = cells_per_process * uint64_t(comm.size()) - this->grid_length;

		#ifndef USE_SFC

		uint64_t cell_to_create = 1;
		for (int process = 0; process < comm.size(); process++) {

			uint64_t cells_to_create;
			if ((unsigned int)process < procs_with_fewer) {
				cells_to_create = cells_per_process - 1;
			} else {
				cells_to_create = cells_per_process;
			}

			for (uint64_t i = 0; i < cells_to_create; i++) {
				this->cell_process[cell_to_create] = process;
				if (process == comm.rank()) {
					this->cells[cell_to_create];
				}
				cell_to_create++;
			}
		}
		assert(cell_to_create == this->grid_length + 1);

		#else

		const dccrg::Types<3>::indices_t length = {
			this->x_length,
			this->y_length,
			this->z_length
		};
		sfc::Sfc<3, uint64_t> mapping(length);

		/*
		Cache only batch_size number of sfc indices at a time.
		Saves memory and can even be faster than caching everything at once
		*/
		uint64_t batch_size;
		if (mapping.size() % sfc_caching_batches > 0) {
			batch_size = 1 + mapping.size() / sfc_caching_batches;
		} else {
			batch_size = mapping.size() / sfc_caching_batches;
		}

		uint64_t cache_start = 0, cache_end = batch_size - 1;
		mapping.cache_sfc_index_range(cache_start, cache_end);

		uint64_t sfc_index = 0;
		for (int process = 0; process < comm.size(); process++) {

			uint64_t cells_to_create;
			if ((unsigned int)process < procs_with_fewer) {
				cells_to_create = cells_per_process - 1;
			} else {
				cells_to_create = cells_per_process;
			}

			for (uint64_t i = 0; i < cells_to_create; i++) {

				// cache new sfc index batch
				if (sfc_index > cache_end) {
					cache_start = cache_end;
					cache_end = cache_start + batch_size;

					if (cache_end >= mapping.size()) {
						cache_end = mapping.size() - 1;
					}

					mapping.clear();
					mapping.cache_sfc_index_range(cache_start, cache_end);
				}

				dccrg::Types<3>::indices_t indices = mapping.get_indices(sfc_index);
				// transform indices to those of refinement level 0 cells
				indices[0] *= uint64_t(1) << this->max_refinement_level;
				indices[1] *= uint64_t(1) << this->max_refinement_level;
				indices[2] *= uint64_t(1) << this->max_refinement_level;
				const uint64_t cell_to_create = this->get_cell_from_indices(indices, 0);

				this->cell_process[cell_to_create] = process;
				if (process == this->comm.rank()) {
					this->cells[cell_to_create];
				}

				sfc_index++;
			}
		}
		mapping.clear();
		assert(sfc_index == this->grid_length);

		#endif

		// update neighbour lists of created cells
		BOOST_FOREACH(cell_and_data_pair_t item, this->cells) {
			this->neighbours[item.first] = this->find_neighbours_of(item.first);
			this->neighbours_to[item.first] = this->find_neighbours_to(item.first);
		}
		#ifdef DEBUG
		if (!this->verify_neighbours()) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Neighbour lists are inconsistent" << std::endl;
			// TODO: throw an exception instead when debugging?
			exit(EXIT_FAILURE);
		}
		#endif

		BOOST_FOREACH(cell_and_data_pair_t item, this->cells) {
			this->update_remote_neighbour_info(item.first);
		}
		#ifdef DEBUG
		if (!this->verify_remote_neighbour_info()) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Remote neighbour info is not consistent" << std::endl;
			exit(EXIT_FAILURE);
		}
		#endif

		this->recalculate_neighbour_update_send_receive_lists();

		this->initialized = true;
	}


	/*!
	Returns all cells on this process that don't have children (e.g. leaf cells)
	*/
	std::vector<uint64_t> get_cells(void) const
	{
		std::vector<uint64_t> all_cells;
		all_cells.reserve(this->cells.size());

		BOOST_FOREACH(cell_and_data_pair_t item, this->cells) {

			#ifdef DEBUG
			if (this->cell_process.count(item.first) == 0) {
				std::cerr << __FILE__ << ":" << __LINE__
					<< " Cell " << item.first
					<< " shouldn't exist"
					<< std::endl;
				abort();
			}

			if (this->cell_process.at(item.first) != this->comm.rank()) {
				std::cerr << __FILE__ << ":" << __LINE__
					<< " Process " << this->comm.rank()
					<< ": Cell " << item.first
					<< " should be on process " << this->cell_process.at(item.first)
					<< std::endl;
				abort();
			}

			const uint64_t child = this->get_child(item.first);
			if (child == 0) {
				std::cerr << __FILE__ << ":" << __LINE__
					<< " Process " << this->comm.rank()
					<< ": Child == 0 for cell " << item.first
					<< std::endl;
				abort();
			}

			if (child != item.first) {
				std::cerr << __FILE__ << ":" << __LINE__
					<< " Process " << this->comm.rank()
					<< ": Cell " << item.first
					<< " has a child"
					<< std::endl;
				abort();
			}
			#endif

			all_cells.push_back(item.first);
		}

		return all_cells;
	}

	/*!
	Returns a begin const_iterator to the internal storage of local cells and their data.
	*/
	typename boost::unordered_map<uint64_t, UserData>::const_iterator begin(void) const
	{
		return this->cells.begin();
	}

	/*!
	Returns an end const_iterator to the internal storage of local cells and their data.
	*/
	typename boost::unordered_map<uint64_t, UserData>::const_iterator end(void) const
	{
		return this->cells.end();
	}


	/*!
	Returns the number of local cells without children, e.g. leaf cells.
	*/
	size_t size(void) const
	{
		return this->cells.size();
	}


	/*!
	Returns all cells on this process that don't have children (e.g. leaf cells) and don't have neighbours on other processes
	*/
	std::vector<uint64_t> get_cells_with_local_neighbours(void) const
	{
		std::vector<uint64_t> return_cells;
		return_cells.reserve(this->cells.size());

		BOOST_FOREACH(cell_and_data_pair_t item, this->cells) {

			uint64_t child = this->get_child(item.first);
			assert(child > 0);

			if (child != item.first) {
				continue;
			}

			bool has_remote_neighbour = false;

			assert(this->neighbours.count(item.first) > 0);

			BOOST_FOREACH(uint64_t neighbour, this->neighbours.at(item.first)) {

				if (neighbour == 0) {
					continue;
				}

				if (this->cell_process.at(neighbour) != this->comm.rank()) {
					has_remote_neighbour = true;
					break;
				}
			}

			if (!has_remote_neighbour) {
				return_cells.push_back(item.first);
			}
		}

		return return_cells;
	}


	/*!
	Returns all cells on this process that don't have children (e.g. leaf cells) and have at least one neighbour on another processes
	*/
	std::vector<uint64_t> get_cells_with_remote_neighbour(void) const
	{
		std::vector<uint64_t> return_cells;
		return_cells.reserve(this->cells.size());

		BOOST_FOREACH(cell_and_data_pair_t item, this->cells) {

			uint64_t child = this->get_child(item.first);
			assert(child > 0);

			if (child != item.first) {
				continue;
			}

			bool has_remote_neighbour = false;

			assert(this->neighbours.count(item.first) > 0);

			BOOST_FOREACH(uint64_t neighbour, this->neighbours.at(item.first)) {

				if (neighbour == 0) {
					continue;
				}

				if (this->cell_process.at(neighbour) != this->comm.rank()) {
					has_remote_neighbour = true;
					break;
				}
			}

			if (has_remote_neighbour) {
				return_cells.push_back(item.first);
			}
		}

		return return_cells;
	}


	/*!
	Returns all cells in the grid that don't have children (e.g. leaf cells)
	*/
	std::vector<uint64_t> get_all_cells(void) const
	{
		std::vector<uint64_t> all_cells;
		all_cells.reserve(this->cell_process.size());

		for (boost::unordered_map<uint64_t, int>::const_iterator
			item = this->cell_process.begin();
			item != this->cell_process.end();
			item++
		) {

			const uint64_t child = this->get_child(item->first);

			if (child == item->first) {
				all_cells.push_back(item->first);
			}
		}

		return all_cells;
	}


	/*!
	Returns a pointer to the user supplied data of given cell
	Return NULL if the given cell isn't on this process and if the given cell isn't a neighbour of any cell on this process
	*/
	UserData* operator [] (const uint64_t cell) const
	{
		if (this->cells.count(cell) > 0) {
			return (UserData*) &(this->cells.at(cell));
		} else if (this->remote_neighbours.count(cell) > 0) {
			return (UserData*) &(this->remote_neighbours.at(cell));
		} else if (this->refined_cell_data.count(cell) > 0) {
			return (UserData*) &(this->refined_cell_data.at(cell));
		} else if (this->unrefined_cell_data.count(cell) > 0) {
			return (UserData*) &(this->unrefined_cell_data.at(cell));
		} else {
			return NULL;
		}
	}


	/*!
	Refines the grid so that at least the given cells whose parents are on this process will exist in the grid.

	Must be called simultaneously on all processes.
	Does not store the user data of any refined cell.
	Returns true on this process if successful and false if given an invalid cell (0 or a cell with a too large refinement level).
	*/
	bool load(const std::vector<uint64_t>& cells)
	{
		// see for example http://www.informit.com/articles/article.aspx?p=376878&rll=1 for an explanation about template<template...
		this->comm.barrier();

		// calculate which cells must be refined...
		boost::unordered_set<uint64_t> cells_to_refine;

		// ...and check for invalid cells
		BOOST_FOREACH(uint64_t cell, cells) {

			if (cell == 0) {
				return false;
			}

			if (this->get_refinement_level(cell) < 0) {
				return false;
			}

			// refine all parents of current cell
			uint64_t parent = this->get_parent_for_removed(cell);
			while (parent != this->get_parent_for_removed(parent)) {
				cells_to_refine.insert(parent);
			}
			cells_to_refine.insert(parent);
		}

		// keep refining until no more refines on any process, TODO

		return true;
	}


	/*!
	\brief Load balances the grid's cells among processes.

	Must be called simultaneously on all processes.
	Cells which haven't been pinned are moved as suggested by Zoltan, pinned cells are moved as requested by the user.
	Does not update remote neighbour data between processes afterward.
	Discards refines / unrefines.

	If prepare_to_balance_load was called before this then has_been_prepared must be true
	otherwise it must be false.
	*/
	void balance_load(const bool has_been_prepared = false)
	{
		if (!has_been_prepared) {
			this->make_new_partition(true);
		}
		this->move_cells();
		this->added_cells.clear();
		this->removed_cells.clear();
	}

	/*!
	Moves pinned grid cells as requested by the user.

	Must be called simultaneously on all processes.
	Cells which haven't been pinned are not moved.
	Does not update remote neighbour data between processes afterward.
	Discards refines / unrefines.

	If prepare_to_migrate_cells was called before this then
	has_been_prepared must be true otherwise it must be false.
	*/
	void migrate_cells(const bool has_been_prepared = false)
	{
		if (!has_been_prepared) {
			this->make_new_partition(false);
		}
		this->move_cells();
		this->added_cells.clear();
		this->removed_cells.clear();
	}


	/*!
	Same as balance_load but only prepares to move cells with balance_load.

	Must be used when cells contain variable mpi_datatypes so that when
	cells are moved receiving processes can construct the receiving
	datatypes in balance_load based on cells data transferred by this function.
	The next dccrg function to be called after this one must be balance_load.

	When calling balance_load after this function has_been_prepared must be true.
	*/
	void prepare_to_balance_load(void)
	{
		this->make_new_partition(true);
		this->prepare_to_move_cells();
	}

	/*!
	Same as migrate_cells but only prepares to move cells with migrate_cells.

	Must be used when cells contain variable mpi_datatypes so that when
	cells are moved receiving processes can construct the receiving
	datatypes in migrate_cells based on cells data transferred by this function.
	The next dccrg function to be called after this one must be migrate_cells.

	When calling migrate_cells after this function has_been_prepared must be true.
	*/
	void prepare_to_migrate_cells(void)
	{
		this->make_new_partition(true);
		this->prepare_to_move_cells();
	}


	/*!
	\brief Updates the user data of neighboring cells between processes.

	User data of any local cell which is considered as a neighbor
	to a cell on another process is sent to that process.
	User data of any cell that is considered as a neighbor of a
	cell on this process is received from the other process.
	Cells' user data is only sent to / received from a process once.
	Must be called simultaneously on all processes
	*/
	void update_remote_neighbour_data(void)
	{
		this->start_remote_neighbour_data_update();
		this->wait_neighbour_data_update();
	}


	/*!
	Starts the update of neighbour data between processes and returns before (probably) it has completed
	Must be called simultaneously on all processes
	*/
	void start_remote_neighbour_data_update(void)
	{
		this->comm.barrier();
		this->start_user_data_transfers(
		#ifdef DCCRG_SEND_SINGLE_CELLS
		this->remote_neighbours
		#else
		#ifdef DCCRG_CELL_DATA_SIZE_FROM_USER
		this->remote_neighbours
		#endif
		#endif
		);
	}


	/*!
	Waits until all neighbour data update transfers between processes have completed and incorporates that data.
	Must be called simultaneously on all processes.
	*/
	void wait_neighbour_data_update(void)
	{
		this->wait_neighbour_data_update_receives();
		this->wait_neighbour_data_update_sends();
	}


	/*!
	Waits until all sends associated with neighbour data update transfers between processes have completed.
	Must be called simultaneously on all processes and probably must be called after wait...update_receives(void).
	*/
	void wait_neighbour_data_update_sends(void)
	{
		this->wait_user_data_transfer_sends();
	}


	/*!
	Waits until all receives associated with neighbour data update transfers between processes have completed and incorporates that data.
	Must be called simultaneously on all processes and probably must be called before wait...update_sends(void).
	*/
	void wait_neighbour_data_update_receives(void)
	{
		this->wait_user_data_transfer_receives(
		#ifndef DCCRG_SEND_SINGLE_CELLS
		#ifndef DCCRG_CELL_DATA_SIZE_FROM_USER
		this->remote_neighbours
		#endif
		#endif
		);
	}


	/*!
	Returns the number of cells whose data this process has to send during a neighbour data update.

	The total amount of cells to be sent is returned so if a cell's data will be sent to
	N processes it is counted N times.
	*/
	uint64_t get_number_of_update_send_cells(void) const
	{
		uint64_t result = 0;
		for (
			#ifdef DCCRG_SEND_SINGLE_CELLS
			boost::unordered_map<int, std::vector<std::pair<uint64_t, int> > >::const_iterator
			#else
			boost::unordered_map<int, std::vector<uint64_t> >::const_iterator
			#endif
			receiver = cells_to_send.begin();
			receiver != cells_to_send.end();
			receiver++
		) {
			result += receiver->second.size();
		}
		return result;
	}

	/*!
	Returns the number of cells whose data this process has to receive during a neighbour data update.
	*/
	uint64_t get_number_of_update_receive_cells(void) const
	{
		uint64_t result = 0;
		for (
			#ifdef DCCRG_SEND_SINGLE_CELLS
			boost::unordered_map<int, std::vector<std::pair<uint64_t, int> > >::const_iterator
			#else
			boost::unordered_map<int, std::vector<uint64_t> >::const_iterator
			#endif
			sender = cells_to_receive.begin();
			sender != cells_to_receive.end();
			sender++
		) {
			result += sender->second.size();
		}
		return result;
	}


	/*!
	Returns a pointer to the neighbours of given cell.

	In case the grid is not periodic in one or more directions,
	neighbors that would be outside of the grid are 0.
	Some neighbours might be on another process, but have a copy of their data on this process.
	The local copy of remote neighbours' data is updated, for example, by calling
	update_remote_neighbour_data().
	Returns NULL if given cell doesn't exist or is on another process.
	*/
	const std::vector<uint64_t>* get_neighbours(const uint64_t cell) const
	{
		if (this->cells.count(cell) > 0) {
			#ifdef DEBUG
			if (this->neighbours.count(cell) == 0) {
				std::cerr << __FILE__ << ":" << __LINE__
					<< " Process " << this->comm.rank()
					<< ": Neighbour list for cell " << cell
					<< " doesn't exist"
					<< std::endl;
				abort();
			}
			#endif
			return &(this->neighbours.at(cell));
		} else {
			return NULL;
		}
	}

	/*!
	Returns a pointer to the cells that consider given cell as a neighbour.

	This list doesn't include 0s even if the grid isn't periodic in some direction.
	Returns NULL if given cell doesn't exist or is on another process
	*/
	const std::vector<uint64_t>* get_neighbours2(const uint64_t cell) const
	{
		if (this->cells.count(cell) > 0) {
			#ifdef DEBUG
			if (this->neighbours_to.count(cell) == 0) {
				std::cerr << __FILE__ << ":" << __LINE__
					<< " Neighbours_to list for cell " << cell
					<< " doesn't exist"
					<< std::endl;
				abort();
			}
			#endif
			return &(this->neighbours_to.at(cell));
		} else {
			return NULL;
		}
	}


	/*!
	Returns the size of cells' neihgbourhood in every direction.
	*/
	unsigned int get_neighbourhood_size(void) const
	{
		return this->neighbourhood_size;
	}


	/*!
	Returns all neighbors of given cell that are at given offsets from it.

	Returns nothing in the following cases:
		-given cell doesn't exist
		-given cell is on another process
		-any of given offsets is larger in absolute value than the neighborhood
			size or larger than 1 if neihgborhood size == 0
		- i == 0 && j == 0 && k == 0
	*/
	std::vector<uint64_t> get_neighbors_of(const uint64_t cell, const int i, const int j, const int k) const
	{
		std::vector<uint64_t> return_neighbors;
		if (this->cell_process.count(cell) == 0
		|| this->cell_process.at(cell) != this->comm.rank()
		|| (i == 0 && j == 0 && k == 0)) {
			return return_neighbors;
		}

		const int refinement_level = this->get_refinement_level(cell);

		const int last_offset = (this->neighbourhood_size > 0) ? int(this->neighbourhood_size) : 1;
		int index = 0;
		for (int
			current_k = (this->neighbourhood_size > 0) ? -int(this->neighbourhood_size) : -1;
			current_k <= last_offset;
			current_k++
		)
		for (int
			current_j = (this->neighbourhood_size > 0) ? -int(this->neighbourhood_size) : -1;
			current_j <= last_offset;
			current_j++
		)
		for (int
			current_i = (this->neighbourhood_size > 0) ? -int(this->neighbourhood_size) : -1;
			current_i <= last_offset;
			current_i++
		) {
			if (current_i == 0 && current_j == 0 && current_k == 0) {
				continue;
			}

			if (this->neighbourhood_size == 0) {
				// skip diagonal offsets
				const int zeros_in_current =
					((current_k == 0) ? 1 : 0)
					+ ((current_j == 0) ? 1 : 0)
					+ ((current_i == 0) ? 1 : 0);
				if (zeros_in_current != 2) {
					continue;
				}
			}

			const int current_refinement_level = this->get_refinement_level(this->neighbours.at(cell)[index]);
			if (i == current_i && j == current_j && k == current_k) {

				if (current_refinement_level == -1) {
					return_neighbors.push_back(0);
				} else {
					return_neighbors.push_back(this->neighbours.at(cell)[index]);

					if (current_refinement_level > refinement_level) {
						return_neighbors.reserve(8);
						for (int i = 1; i < 8; i++) {
							index++;
							return_neighbors.push_back(this->neighbours.at(cell)[index]);
						}
					}
				}

				current_i = current_j = current_k = last_offset + 1;

			} else {
				if (current_refinement_level > refinement_level) {
					index += 7;
				}
			}

			index++;
		}

		return return_neighbors;
	}


	/*!
	Returns the given cell's neighbours that are on another process.

	Returns nothing if given cell doesn't exist or is on another process
	or doesn't have remote neighbours.
	*/
	std::vector<uint64_t> get_remote_neighbours(const uint64_t cell) const
	{
		std::vector<uint64_t> result;

		if (this->cells.count(cell) == 0
		|| this->neighbours.count(cell) == 0) {
			return result;
		}

		BOOST_FOREACH(uint64_t neighbour, this->neighbours.at(cell)) {

			if (neighbour == 0) {
				continue;
			}

			if (this->cell_process.at(neighbour) != this->comm.rank()) {
				result.push_back(neighbour);
			}
		}

		return result;
	}


	/*!
	Returns true if given cell is on this process and false otherwise.
	*/
	bool is_local(const uint64_t cell) const
	{
		if (this->cell_process.count(cell) > 0
		&& this->cell_process.at(cell) == this->comm.rank()) {
			return true;
		} else {
			return false;
		}
	}


	/*!
	Writes the cells on this process into a vtk file with given name in ASCII format
	The cells are written in ascending order
	Must be called simultaneously on all processes
	*/
	void write_vtk_file(const char* file_name) const
	{
		std::ofstream outfile(file_name);
		if (!outfile.is_open()) {
			std::cerr << "Couldn't open file " << file_name << std::endl;
			// TODO: throw an exception instead
			exit(1);
		}

		std::vector<uint64_t> leaf_cells = this->get_cells();
		std::sort(leaf_cells.begin(), leaf_cells.end());
		outfile << "# vtk DataFile Version 2.0" << std::endl;
		outfile << "Cartesian cell refinable grid" << std::endl;
		outfile << "ASCII" << std::endl;
		outfile << "DATASET UNSTRUCTURED_GRID" << std::endl;

		// write separate points for every cells corners
		outfile << "POINTS " << leaf_cells.size() * 8 << " float" << std::endl;
		for (unsigned int i = 0; i < leaf_cells.size(); i++) {
			outfile << this->get_cell_x_min(leaf_cells[i]) << " "
				<< this->get_cell_y_min(leaf_cells[i]) << " "
				<< this->get_cell_z_min(leaf_cells[i]) << std::endl;
			outfile << this->get_cell_x_max(leaf_cells[i]) << " "
				<< this->get_cell_y_min(leaf_cells[i]) << " "
				<< this->get_cell_z_min(leaf_cells[i]) << std::endl;
			outfile << this->get_cell_x_min(leaf_cells[i]) << " "
				<< this->get_cell_y_max(leaf_cells[i]) << " "
				<< this->get_cell_z_min(leaf_cells[i]) << std::endl;
			outfile << this->get_cell_x_max(leaf_cells[i]) << " "
				<< this->get_cell_y_max(leaf_cells[i]) << " "
				<< this->get_cell_z_min(leaf_cells[i]) << std::endl;
			outfile << this->get_cell_x_min(leaf_cells[i]) << " "
				<< this->get_cell_y_min(leaf_cells[i]) << " "
				<< this->get_cell_z_max(leaf_cells[i]) << std::endl;
			outfile << this->get_cell_x_max(leaf_cells[i]) << " "
				<< this->get_cell_y_min(leaf_cells[i]) << " "
				<< this->get_cell_z_max(leaf_cells[i]) << std::endl;
			outfile << this->get_cell_x_min(leaf_cells[i]) << " "
				<< this->get_cell_y_max(leaf_cells[i]) << " "
				<< this->get_cell_z_max(leaf_cells[i]) << std::endl;
			outfile << this->get_cell_x_max(leaf_cells[i]) << " "
				<< this->get_cell_y_max(leaf_cells[i]) << " "
				<< this->get_cell_z_max(leaf_cells[i]) << std::endl;
		}

		// map cells to written points
		outfile << "CELLS " << leaf_cells.size() << " " << leaf_cells.size() * 9 << std::endl;
		for (unsigned int j = 0; j < leaf_cells.size(); j++) {
			outfile << "8 ";
			for (int i = 0; i < 8; i++) {
				 outfile << j * 8 + i << " ";
			}
			outfile << std::endl;
		}

		// cell types
		outfile << "CELL_TYPES " << leaf_cells.size() << std::endl;
		for (unsigned int i = 0; i < leaf_cells.size(); i++) {
			outfile << 11 << std::endl;
		}

		if (!outfile.good()) {
			std::cerr << "Writing of vtk file probably failed" << std::endl;
			// TODO: throw an exception instead
			exit(EXIT_FAILURE);
		}

		outfile.close();
	}


	/*!
	Creates all children of given cell (and possibly of other cells due to induced refinement).

	Takes priority over unrefining. Refines / unrefines take effect only after a call to stop_refining() and are lost after a call to balance_load(). Does nothing in any of the following cases:
		-given cell has already been refined (including induced refinement) and stop_refining() has not been called afterwards
		-given cell doesn't exist on this process
		-given cell's children already exist
	Children are created on their parent's process.

	If given cell is at maximum refinement level dont_unrefine will be invoked instead.
	 */
	void refine_completely(const uint64_t cell)
	{
		if (this->cell_process.count(cell) == 0) {
			return;
		}

		if (this->cells.count(cell) == 0) {
			return;
		}

		const int refinement_level = this->get_refinement_level(cell);

		if (refinement_level > this->max_refinement_level) {
			return;
		}

		if (refinement_level == this->max_refinement_level) {
			this->dont_unrefine(cell);
			return;
		}

		if (cell != this->get_child(cell)) {
			// cell already has children
			return;
		}

		this->cells_to_refine.insert(cell);
	}

	/*!
	As refine_completely, but uses the smallest existing cell at given coordinates.
	Does nothing in the same cases as refine_completely and additionally if the coordinate is outside of the grid.
	*/
	void refine_completely_at(const double x, const double y, const double z)
	{
		const uint64_t cell = this->get_existing_cell(x, y, z);
		if (cell == 0) {
			return;
		}

		this->refine_completely(cell);
	}


	/*!
	Removes the given cell and its siblings from the grid.

	Refining (including induced refining) takes priority over unrefining. Refines / unrefines take effect only after a call to stop_refining() and are lost after a call to balance_load(). Does nothing in any of the following cases:
		-dont_unrefine was called previously for given cell or its siblings
		-given cell or one of its siblings has already been unrefined and stop_refining() has not been called
		-given cell doesn't exist on this process
		-given cell has children
		-given cells refinement level is 0

	After a cell and its siblings have been unrefined, their data has been moved to their parent's process. When no longer needed that data can be freed using ?.
	*/
	void unrefine_completely(const uint64_t cell)
	{
		if (this->cell_process.count(cell) == 0) {
			return;
		}

		if (this->cells.count(cell) == 0) {
			return;
		}

		if (this->get_refinement_level(cell) == 0) {
			return;
		}

		if (cell != this->get_child(cell)) {
			// cell already has children
			return;
		}

		// record only one sibling to unrefine / process
		const std::vector<uint64_t> siblings = this->get_all_children(this->get_parent(cell));

		BOOST_FOREACH(uint64_t sibling, siblings) {
			if (this->cells_to_unrefine.count(sibling) > 0) {
				return;
			}

			if (this->cells_not_to_unrefine.count(sibling) > 0) {
				return;
			}
		}

		this->cells_to_unrefine.insert(cell);
	}


	/*!
	As unrefine_completely, but uses the smallest existing cell at given coordinates.
	Does nothing in the same cases as unrefine_completely and additionally if the coordinate is outside of the grid.
	*/
	void unrefine_completely_at(const double x, const double y, const double z)
	{
		const uint64_t cell = this->get_existing_cell(x, y, z);
		if (cell == 0) {
			return;
		}

		this->unrefine_completely(cell);
	}


	/*!
	Prevents the given cell or its siblings from being unrefined.

	Has an effect only during the next call to stop_refining().
	Has no effect if balance_load() is called before stop_refining().
	Does nothing in any of the following cases:
		-given cell doesn't exist on this process
		-given cell has children
		-given cell's refinement level is 0
	*/
	void dont_unrefine(const uint64_t cell)
	{
		if (this->cell_process.count(cell) == 0) {
			return;
		}

		if (this->cells.count(cell) == 0) {
			return;
		}

		if (this->get_refinement_level(cell) == 0) {
			return;
		}

		if (cell != this->get_child(cell)) {
			// cell already has children
			return;
		}

		// record only one sibling / process
		const std::vector<uint64_t> siblings = this->get_all_children(this->get_parent(cell));

		BOOST_FOREACH(uint64_t sibling, siblings) {
			if (this->cells_not_to_unrefine.count(sibling) > 0) {
				return;
			}

			// remove siblings from local unrefines
			this->cells_to_unrefine.erase(sibling);
		}

		this->cells_not_to_unrefine.insert(cell);
	}


	/*!
	As dont_unrefine but uses the smallest existing cell at given coordinates.

	Does nothing in the same cases as dont_unrefine and additionally if the
	coordinate is outside of the grid.
	*/
	void dont_unrefine_at(const double x, const double y, const double z)
	{
		const uint64_t cell = this->get_existing_cell(x, y, z);
		if (cell == error_cell) {
			return;
		}

		this->dont_unrefine(cell);
	}


	/*!
	Executes refines / unrefines that have been requested so far.

	Must be called simultaneously on all processes.
	Returns cells that were created by refinement on this process.
	Moves user data of unrefined cells to the process of their parent.
	*/
	std::vector<uint64_t> stop_refining(void)
	{
		this->comm.barrier();
		this->induce_refines();

		// update dont_refines between processes
		this->all_to_all_set(this->cells_not_to_unrefine);

		this->override_unrefines();
		this->cells_not_to_unrefine.clear();

		return this->execute_refines();
	}


	/*!
	Returns cells that were removed by unrefinement whose parent is on this process
	Removed cells data is also on this process, but only until balance_load() is called
	*/
	std::vector<uint64_t> get_removed_cells(void) const
	{
		std::vector<uint64_t> unref_removed_cells;
		unref_removed_cells.reserve(this->unrefined_cell_data.size());

		BOOST_FOREACH(cell_and_data_pair_t item, this->unrefined_cell_data) {
			unref_removed_cells.push_back(item.first);
		}

		return unref_removed_cells;
	}


	/*!
	Given a cell that exists and has a parent returns the parent cell
	Returns the given cell if it doesn't have a parent or 0 if given cell doesn't exist
	*/
	uint64_t get_parent(const uint64_t cell) const
	{
		if (this->cell_process.count(cell) == 0) {
			return 0;
		}

		// given cell cannot have a parent
		if (this->get_refinement_level(cell) == 0) {
			return cell;
		}

		const uint64_t parent = this->get_cell_from_indices(
			this->get_indices(cell),
			this->get_refinement_level(cell) - 1
		);

		if (this->cell_process.count(parent) > 0) {
			return parent;
		} else {
			return cell;
		}
	}


	/*!
	Returns the indices corresponding to the given neighbourhood (with given neighbour size in indices) at given indices.

	If grid is not periodic those indices will be error_indices that would fall outside of the grid.
	*/
	std::vector<Types<3>::indices_t> indices_from_neighbourhood(
		const Types<3>::indices_t indices,
		const uint64_t size_in_indices,
		const std::vector<Types<3>::neighbourhood_item_t>* neighbourhood
	) const
	{
		std::vector<Types<3>::indices_t> return_indices;
		return_indices.reserve(neighbourhood->size());

		// grid length in indices
		const uint64_t grid_length[3] = {
			this->get_x_length() * (uint64_t(1) << this->max_refinement_level),
			this->get_y_length() * (uint64_t(1) << this->max_refinement_level),
			this->get_z_length() * (uint64_t(1) << this->max_refinement_level)
		};

		#ifdef DEBUG
		for (unsigned int dimension = 0; dimension < 3; dimension++) {
			if (indices[dimension] >= grid_length[dimension]) {
				std::cerr << __FILE__ << ":" << __LINE__
					<< " Given indices outside of the grid in dimension " << dimension
					<< std::endl;
				abort();
			}
		}
		#endif

		BOOST_FOREACH(Types<3>::neighbourhood_item_t offsets, *neighbourhood) {

			Types<3>::indices_t temp_indices = { indices[0], indices[1], indices[2] };

			for (unsigned int dimension = 0; dimension < 3; dimension++) {
				if (offsets[dimension] < 0) {

					if (this->periodic[dimension]) {

						// neighbourhood might wrap around the grid several times
						for (int i = 0; i > offsets[dimension]; i--) {

							#ifdef DEBUG
							if (temp_indices[dimension] < size_in_indices - 1
							&& temp_indices[dimension] > 0) {
								std::cerr << __FILE__ << ":" << __LINE__
									<< " Cells aren't supposed to wrap around the grid."
									<< std::endl;
								abort();
							}
							#endif

							if (temp_indices[dimension] >= size_in_indices) {
								temp_indices[dimension] -= size_in_indices;
							} else {
								temp_indices[dimension] = grid_length[dimension] - size_in_indices;
							}
						}
					// use error_indices to signal that this neighbourhood item is outside of the grid
					} else {
						if (indices[dimension] < abs(offsets[dimension]) * size_in_indices) {
							temp_indices[0] = error_index;
							temp_indices[1] = error_index;
							temp_indices[2] = error_index;
							break;
						}

						temp_indices[dimension] += offsets[dimension] * size_in_indices;
					}

				} else {

					if (this->periodic[dimension]) {
						for (int i = 0; i < offsets[dimension]; i++) {

							#ifdef DEBUG
							if (temp_indices[dimension] > grid_length[dimension] - size_in_indices) {
								std::cerr << __FILE__ << ":" << __LINE__ << " Cells aren't supposed to wrap around the grid." << std::endl;
								abort();
							}
							#endif

							if (temp_indices[dimension] < grid_length[dimension] - size_in_indices) {
								temp_indices[dimension] += size_in_indices;
							} else {
								temp_indices[dimension] = 0;
							}
						}
					} else {
						if (indices[dimension] + offsets[dimension] * size_in_indices >= grid_length[dimension]) {
							temp_indices[0] = error_index;
							temp_indices[1] = error_index;
							temp_indices[2] = error_index;
							break;
						}

						temp_indices[dimension] += offsets[dimension] * size_in_indices;
					}
				}
			}

			return_indices.push_back(temp_indices);
		}

		return return_indices;
	}


	/*!
	Returns the existing neighbours (that don't have children) of given cell even if it is on another process.

	Returns nothing if the following cases:
		-given cell has children
		-given doesn't exist on any process
	Doesn't use existing neighbour lists and hence is slow but works if for example given cell was moved to another process by load balancing.
	*/
	std::vector<uint64_t> find_neighbours_of(const uint64_t cell) const
	{
		std::vector<uint64_t> return_neighbours;

		const int refinement_level = this->get_refinement_level(cell);

		#ifdef DEBUG
		if (cell == 0) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Invalid cell given: " << cell << std::endl;
			abort();
		}

		if (refinement_level > this->max_refinement_level) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Refinement level of given cell (" << cell << ") is too large: " << refinement_level << std::endl;
			abort();
		}

		if (refinement_level < 0) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Invalid refinement level for cell " << cell << ": " << refinement_level << std::endl;
			abort();
		}
		#endif

		if (this->cell_process.count(cell) == 0) {
			return return_neighbours;
		}

		if (cell != this->get_child(cell)) {
			return return_neighbours;
		}

		const std::vector<Types<3>::indices_t> indices_of = this->indices_from_neighbourhood(
			this->get_indices(cell),
			this->get_cell_size_in_indices(cell),
			&(this->neighbourhood_of)
		);

		BOOST_FOREACH(Types<3>::indices_t index_of, indices_of) {

			if (index_of[0] == error_index) {
				return_neighbours.push_back(0);
				continue;
			}

			const uint64_t neighbor = this->get_existing_cell(
				index_of,
				(refinement_level == 0)
					? 0 : refinement_level - 1,
				(refinement_level < this->max_refinement_level)
					? refinement_level + 1 : this->max_refinement_level
			);

			#ifdef DEBUG
			if (neighbor == 0) {
				std::cerr << __FILE__ << ":" << __LINE__
					<< " Neighbor not found for cell " << cell
					<< " (ref. lvl. " << refinement_level
					<< ") within refinement levels [" << refinement_level - 1
					<< ", " << refinement_level + 1 << "]"
					<< std::endl;
				abort();
			}
			#endif

			const int neighbor_ref_lvl = this->get_refinement_level(neighbor);

			#ifdef DEBUG
			if (neighbor_ref_lvl < 0) {
				std::cerr << __FILE__ << ":" << __LINE__
					<< " Invalid refinement level for neighbor " << neighbor
					<< " of cell " << cell
					<< std::endl;
				abort();
			}
			#endif

			if (neighbor_ref_lvl <= refinement_level) {
				return_neighbours.push_back(neighbor);
			// add all cells at current search indices within size of given cell
			} else {
				std::vector<uint64_t> siblings = this->get_siblings(neighbor);

				#ifdef DEBUG
				if (siblings.size() == 0) {
					std::cerr << __FILE__ << ":" << __LINE__
						<< " No siblings for neighbor " << neighbor
						<< " of cell " << cell
						<< " at indices " << index_of[0]
						<< ", " << index_of[1]
						<< ", " << index_of[2]
						<< std::endl;
					abort();
				}
				#endif

				return_neighbours.insert(
					return_neighbours.end(),
					siblings.begin(),
					siblings.end()
				);
			}
		}

		return return_neighbours;
	}



	/*!
	Returns cells (which don't have children) that consider given cell as a neighbour.

	Returns nothing if the following cases:
		-given cell has children
		-given cell doesn't exist on any process
	Doesn't use existing neighbour lists and hence is slow but works if for example given cell was moved to another process by load balancing.
	Returned cells might not be in any particular order.
	Assumes a maximum refinement level difference of one between neighbours (both cases: neighbours_of, neighbours_to).
	*/
	std::vector<uint64_t> find_neighbours_to(const uint64_t cell) const
	{
		std::vector<uint64_t> return_neighbours;

		if (cell == 0
		|| cell > this->last_cell
		|| this->cell_process.count(cell) == 0
		|| cell != this->get_child(cell)) {
			return return_neighbours;
		}

		const int refinement_level = this->get_refinement_level(cell);

		#ifdef DEBUG
		if (refinement_level > this->max_refinement_level) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Refinement level (" << refinement_level << ") of cell " << cell << " exceeds maximum refinement level of the grid (" << this->max_refinement_level << ")" << std::endl;
			abort();
		}

		if (refinement_level < 0) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Refinement level of cell " << cell << " is less than 0: " << refinement_level << std::endl;
			abort();
		}
		#endif

		boost::unordered_set<uint64_t> unique_neighbours;

		// neighbours_to larger than given cell
		if (refinement_level > 0) {
			const uint64_t parent = this->get_parent(cell);
			const Types<3>::indices_t indices = this->get_indices(parent);
			const uint64_t size_in_indices = this->get_cell_size_in_indices(parent);

			std::vector<Types<3>::indices_t> search_indices = this->indices_from_neighbourhood(
				indices,
				size_in_indices,
				&(this->neighbourhood_to)
			);

			BOOST_FOREACH(Types<3>::indices_t search_index, search_indices) {

				if (search_index[0] == error_index) {
					continue;
				}

				const uint64_t found = this->get_cell_from_indices(search_index, refinement_level - 1);
				// only add if found cell doesn't have children
				if (found == this->get_child(found)) {
					unique_neighbours.insert(found);
				}
			}
		}

		// neighbours_to smaller than given cell
		if (refinement_level < this->max_refinement_level) {

			const std::vector<uint64_t> children = this->get_all_children(cell);
			#ifdef DEBUG
			if (children.size() == 0) {
				std::cerr << __FILE__ << ":" << __LINE__ << " Got no children for cell " << cell << std::endl;
				abort();
			}
			#endif

			const uint64_t size_in_indices = this->get_cell_size_in_indices(children[0]);

			BOOST_FOREACH(uint64_t child, children) {

				const Types<3>::indices_t indices = this->get_indices(child);

				std::vector<Types<3>::indices_t> search_indices = this->indices_from_neighbourhood(
					indices,
					size_in_indices,
					&(this->neighbourhood_to)
				);

				BOOST_FOREACH(Types<3>::indices_t search_index, search_indices) {

					if (search_index[0] == error_index) {
						continue;
					}

					const uint64_t found = this->get_cell_from_indices(search_index, refinement_level + 1);

					if (found == this->get_child(found)) {
						unique_neighbours.insert(found);
					}
				}
			}
		}

		// neighbours_to of the same size as given cell
		const Types<3>::indices_t indices = this->get_indices(cell);
		const uint64_t size_in_indices = this->get_cell_size_in_indices(cell);

		std::vector<Types<3>::indices_t> search_indices = this->indices_from_neighbourhood(
			indices,
			size_in_indices,
			&(this->neighbourhood_to)
		);

		BOOST_FOREACH(Types<3>::indices_t search_index, search_indices) {

			if (search_index[0] == error_index) {
				continue;
			}

			const uint64_t found = this->get_cell_from_indices(search_index, refinement_level);
			if (found == this->get_child(found)) {
				unique_neighbours.insert(found);
			}
		}

		return_neighbours.reserve(unique_neighbours.size());

		BOOST_FOREACH(uint64_t neighbour, unique_neighbours) {
			return_neighbours.push_back(neighbour);
		}

		return return_neighbours;
	}


	/*!
	As find_neighbours_to(cell) but uses the given neighbours_of list of given cell for finding small
	enough neighbours_to of that cell.
	*/
	std::vector<uint64_t> find_neighbours_to(
		const uint64_t cell,
		const std::vector<uint64_t>& found_neighbours_of
	) const
	{
		std::vector<uint64_t> return_neighbours;

		if (cell == 0
		|| cell > this->last_cell
		|| this->cell_process.count(cell) == 0
		|| cell != this->get_child(cell)) {
			return return_neighbours;
		}

		// get neighbors_to of given cell, first from its neighbors_of
		boost::unordered_set<uint64_t> unique_neighbors_to;

		BOOST_FOREACH(uint64_t neighbor_of, found_neighbours_of) {
			// neighbors_to doesn't store cells that would be outside of the grid
			if (neighbor_of == 0) {
				continue;
			}

			if (this->is_neighbour(neighbor_of, cell)) {
				unique_neighbors_to.insert(neighbor_of);
			}
		}

		const int refinement_level = this->get_refinement_level(cell);
		#ifdef DEBUG
		if (refinement_level > this->max_refinement_level) {
			std::cerr << __FILE__ << ":" << __LINE__
				<< " Refinement level (" << refinement_level
				<< ") of cell " << cell
				<< " exceeds maximum refinement level of the grid (" << this->max_refinement_level << ")"
				<< std::endl;
			abort();
		}

		if (refinement_level < 0) {
			std::cerr << __FILE__ << ":" << __LINE__
				<< " Refinement level of cell " << cell
				<< " is less than 0: " << refinement_level
				<< std::endl;
			abort();
		}
		#endif

		// find cells larger than given cell for neighbors_to list
		if (refinement_level > 0) {

			const uint64_t parent = this->get_parent(cell);
			#ifdef DEBUG
			if (parent == cell) {
				std::cerr << __FILE__ << ":" << __LINE__ << " Invalid parent for cell " << cell << std::endl;
				abort();
			}
			#endif

			const Types<3>::indices_t indices = this->get_indices(parent);
			const uint64_t size_in_indices = this->get_cell_size_in_indices(parent);
			const std::vector<Types<3>::indices_t> search_indices = this->indices_from_neighbourhood(
				indices,
				size_in_indices,
				&(this->neighbourhood_to)
			);

			BOOST_FOREACH(Types<3>::indices_t search_index, search_indices) {

				if (search_index[0] == error_index) {
					continue;
				}

				const uint64_t found = this->get_cell_from_indices(search_index, refinement_level - 1);
				// only add if found cell doesn't have children
				if (found == this->get_child(found)) {
					unique_neighbors_to.insert(found);
				}
			}
		}

		return_neighbours.reserve(unique_neighbors_to.size());
		return_neighbours.insert(
			return_neighbours.begin(),
			unique_neighbors_to.begin(),
			unique_neighbors_to.end()
		);

		return return_neighbours;
	}


	/*!
	Returns unique cells within given rectangular box and refinement levels (both inclusive).

	Cells within given volume are always returned in the following order:
	Starting from the corner closest to the starting corner of the grid cells are returned first in the positive x direction then y direction and finally z direction.
	*/
	std::vector<uint64_t> find_cells
	(
		const Types<3>::indices_t indices_min,
		const Types<3>::indices_t indices_max,
		const int minimum_refinement_level,
		const int maximum_refinement_level
	) const
	{
		// size of cells in indices of given maximum_refinement_level
		const uint64_t index_increase = uint64_t(1) << (this->max_refinement_level - maximum_refinement_level);

		#ifdef DEBUG
		if (minimum_refinement_level > maximum_refinement_level) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Invalid refinement levels given" << std::endl;
			abort();
		}

		// check that outer shell makes sense
		if (indices_min[0] > indices_max[0]) {
			std::cerr << __FILE__ << ":" << __LINE__ << " minimum x index > maximum x index" << std::endl;
			abort();
		}

		if (indices_min[1] > indices_max[1]) {
			std::cerr << __FILE__ << ":" << __LINE__ << " minimum y index > maximum y index" << std::endl;
			abort();
		}

		if (indices_min[2] > indices_max[2]) {
			std::cerr << __FILE__ << ":" << __LINE__ << " minimum z index > maximum z index" << std::endl;
			abort();
		}
		#endif

		std::vector<uint64_t> result;
		boost::unordered_set<uint64_t> uniques;

		Types<3>::indices_t indices = {0, 0, 0};
		for (indices[2] = indices_min[2]; indices[2] <= indices_max[2]; indices[2] += index_increase)
		for (indices[1] = indices_min[1]; indices[1] <= indices_max[1]; indices[1] += index_increase)
		for (indices[0] = indices_min[0]; indices[0] <= indices_max[0]; indices[0] += index_increase) {

			const uint64_t cell = this->get_cell_from_indices(indices, minimum_refinement_level, maximum_refinement_level);

			#ifdef DEBUG
			if (cell == 0) {
				std::cerr << __FILE__ << ":" << __LINE__ << " No cell found between refinement levels [" << minimum_refinement_level << ", " << maximum_refinement_level << "] at indices " << indices[0] << " " << indices[1] << " " << indices[2] << std::endl;
				const uint64_t smallest = this->get_cell_from_indices(indices, 0, this->max_refinement_level);
				std::cerr << __FILE__ << ":" << __LINE__ << " smallest cell there is " << smallest << " with refinement level " << this->get_refinement_level(smallest) << std::endl;
				abort();
			}

			if (cell > this->last_cell) {
				std::cerr << __FILE__ << ":" << __LINE__ << " Cell can't exist" << std::endl;
				abort();
			}
			#endif

			/*
			When searching for neighbours_to, cells may exist with larger refinement level than given in find_neighbours_to and shouldn't be considered.
			*/
			if (cell != this->get_child(cell)) {
				continue;
			}

			// don't add the same cell twice
			if (uniques.count(cell) == 0) {
				uniques.insert(cell);
				result.push_back(cell);
			}
		}

		return result;
	}


	/*!
	Removes user data of refined and unrefined cells from this process.
	*/
	void clear_refined_unrefined_data(void)
	{
		this->refined_cell_data.clear();
		this->unrefined_cell_data.clear();
	}


	/*!
	Sets the given option for non-hierarchial partitioning.

	Does nothing if option name is one of: RETURN_LISTS, EDGE_WEIGHT_DIM, NUM_GID_ENTRIES, OBJ_WEIGHT_DIM
	Call this with name = LB_METHOD and value = HIER to use hierarchial partitioning and set those options using the other function with this name.
	*/
	void set_partitioning_option(const std::string name, const std::string value)
	{
		if (this->reserved_options.count(name) > 0) {
			#ifdef DEBUG
			std::cerr << "User tried to set an option reserved for dccrg (" << name << ": " << value << ")" << std::endl;
			#endif
			return;
		}

		Zoltan_Set_Param(this->zoltan, name.c_str(), value.c_str());
	}


	/*!
	Adds a new level for hierarchial partitioning, with each part of that level having given number of processes.

	Assigns default partitioning options for the added level.
	Does nothing if processes_per_part < 1.
	*/
	void add_partitioning_level(const int processes)
	{
		if (processes < 1) {
			#ifdef DEBUG
			std::cerr << "User tried to assign " << processes << " processes per part for a new hierarchial partitioning level" << std::endl;
			#endif
			return;
		}

		this->processes_per_part.push_back(processes);

		// create default partitioning options for the level
		boost::unordered_map<std::string, std::string> default_load_balance_options;
		default_load_balance_options["LB_METHOD"] = "HYPERGRAPH";
		default_load_balance_options["PHG_CUT_OBJECTIVE"] = "CONNECTIVITY";
		this->partitioning_options.push_back(default_load_balance_options);
	}


	/*!
	Rremoves the given hierarhchial partitioning level.

	Level numbering starts from 0.
	Does nothing if given level doesn't exist.
	*/
	void remove_partitioning_level(const int hierarchial_partitioning_level)
	{
		if (hierarchial_partitioning_level < 0
		|| hierarchial_partitioning_level >= int(this->processes_per_part.size())) {
			return;
		}

		this->processes_per_part.erase(this->processes_per_part.begin() + hierarchial_partitioning_level);
		this->partitioning_options.erase(this->partitioning_options.begin() + hierarchial_partitioning_level);
	}


	/*!
	Adds (or overwrites) the given option and its value for hierarchial partitioning of given level.

	Level numbering starts from 0.
	Does nothing in the following cases:
		option name is one of: RETURN_LISTS, ...;
		given level doesn't exist
	*/
	void add_partitioning_option(const int hierarchial_partitioning_level, const std::string name, const std::string value)
	{
		if (hierarchial_partitioning_level < 0
		|| hierarchial_partitioning_level >= int(this->processes_per_part.size())) {
			return;
		}

		if (this->reserved_options.count(name) > 0) {
			#ifdef DEBUG
			std::cerr << "User tried to set an option reserved for dccrg (" << name << ": " << value << ") for level " << hierarchial_partitioning_level << " of hierarchial partitioning" << std::endl;
			#endif
			return;
		}

		this->partitioning_options[hierarchial_partitioning_level][name] = value;
	}


	/*!
	Removes the given option from the given level of hierarchial partitioning.

	Level numbering starts from 0.
	Does nothing if given level doesn't exist.
	*/
	void remove_partitioning_option(const int hierarchial_partitioning_level, const std::string name)
	{
		if (hierarchial_partitioning_level < 0
		|| hierarchial_partitioning_level >= int(this->processes_per_part.size())) {
			return;
		}

		this->partitioning_options[hierarchial_partitioning_level].erase(name);
	}


	/*!
	Returns the names of partitioning options for hierarchial partitioning at given level.

	Returns nothing if given level doesn't exist.
	*/
	std::vector<std::string> get_partitioning_options(const int hierarchial_partitioning_level) const
	{
		std::vector<std::string> partitioning_options;

		if (hierarchial_partitioning_level < 0
		|| hierarchial_partitioning_level >= int(this->processes_per_part.size())) {
			return partitioning_options;
		}

		for (boost::unordered_map<std::string, std::string>::const_iterator
			option = this->partitioning_options.at(hierarchial_partitioning_level).begin();
			option != this->partitioning_options.at(hierarchial_partitioning_level).end();
			option++
		) {
			partitioning_options.push_back(option->first);
		}

		return partitioning_options;
	}


	/*!
	Returns the value of given non-hierarchial partitioning option.

	Returns an empty string if given option or given level doesn't exist.
	*/
	std::string get_partitioning_option_value(const int hierarchial_partitioning_level, const std::string name) const
	{
		std::string value;

		if (hierarchial_partitioning_level < 0
		|| hierarchial_partitioning_level >= int(this->processes_per_part.size())) {
			return value;
		}

		if (this->partitioning_options.count(name) > 0) {
			value = this->partitioning_options.at(name);
		}

		return value;
	}


	/*!
	Returns the process which has the given cell or -1 if the cell doesn't exist.
	*/
	int get_process(const uint64_t cell) const
	{
		if (this->cell_process.count(cell) == 0) {
			return -1;
		} else {
			return this->cell_process.at(cell);
		}
	}


	/*!
	Given cell is kept on this process during subsequent load balancing.

	Does nothing in the same cases as pin(cell, process).
	*/
	void pin(const uint64_t cell)
	{
		this->pin(cell, this->comm.rank());
	}

	/*!
	Given cell is sent to the given process and kept there during subsequent load balancing.

	Does nothing in the following cases:
		-given cell doesn't exist
		-given cell exists on another process
		-given cell has children
		-given process doesn't exist
	*/
	void pin(const uint64_t cell, const int process)
	{
		if (this->cell_process.count(cell) == 0) {
			return;
		}

		if (this->cell_process.at(cell) != this->comm.rank()) {
			return;
		}

		if (cell != this->get_child(cell)) {
			return;
		}

		if (process < 0 || process >= this->comm.size()) {
			return;
		}

		// do nothing if the request already exists
		if (this->pin_requests.count(cell) > 0
		&& this->pin_requests.at(cell) == process) {
			return;
		}

		this->new_pin_requests[cell] = process;
	}

	/*!
	Allows the given cell to be moved to another process during subsequent load balancing.

	Does nothing in the following cases:
		-given cell has children
		-given cell doesn't exist
		-given cell exists on another process
	*/
	void unpin(const uint64_t cell)
	{
		if (this->cell_process.count(cell) == 0) {
			return;
		}

		if (this->cell_process.at(cell) != this->comm.rank()) {
			return;
		}

		if (cell != this->get_child(cell)) {
			return;
		}

		if (this->pin_requests.count(cell) > 0) {
			this->new_pin_requests[cell] = -1;
		} else {
			this->new_pin_requests.erase(cell);
		}
	}

	/*!
	Executes unpin(cell) for all cells on this process.
	*/
	void unpin_local_cells(void)
	{
		#ifdef DEBUG
		// check that all child cells on this process are also in this->cells.
		for (boost::unordered_map<uint64_t, int>::const_iterator
			i = this->cell_process.begin();
			i != this->cell_process.end();
			i++
		) {
			const uint64_t cell = i->first;

			if (this->cell_process.at(cell) != this->comm.rank()) {
				return;
			}

			if (cell == this->get_child(cell)) {
				if (this->cells.count(cell) == 0) {
					std::cerr << __FILE__ << ":" << __LINE__ << " Cell " << cell << " should be in this->cells of process " << this->comm.rank() << std::endl;
					abort();
				}
			} else {
				if (this->cells.count(cell) > 0) {
					std::cerr << __FILE__ << ":" << __LINE__ << " Cell " << cell << " shouldn't be in this->cells of process " << this->comm.rank() << std::endl;
					abort();
				}
			}
		}
		#endif

		BOOST_FOREACH(cell_and_data_pair_t item, this->cells) {
			this->unpin(item.first);
		}
	}

	/*!
	Allows all cells of all processes to be moved to another process during subsequent load balancing.

	Must be called simultaneously on all processes.
	*/
	void unpin_all_cells(void)
	{
		this->comm.barrier();
		this->new_pin_requests.clear();
		this->pin_requests.clear();
	}


	/*!
	Returns a pointer to the send lists of this process.

	These lists record which cells' user data this process will send during neighbour data updates.
	The key is the target process.
	*/
	#ifdef DCCRG_SEND_SINGLE_CELLS
	const boost::unordered_map<int, std::vector<std::pair<uint64_t, int> > >*
	#else
	const boost::unordered_map<int, std::vector<uint64_t> >*
	#endif
	get_send_lists(void)
	{
		return &(this->cells_to_send);
	}

	/*!
	Returns a pointer to the receive lists of this process.

	These lists record which cells' user data this process will receive during neighbour data updates.
	The key is the source process.
	*/
	#ifdef DCCRG_SEND_SINGLE_CELLS
	const boost::unordered_map<int, std::vector<std::pair<uint64_t, int> > >*
	#else
	const boost::unordered_map<int, std::vector<uint64_t> >*
	#endif
	get_receive_lists(void)
	{
		return &(this->cells_to_receive);
	}


	/*!
	Returns a pointer to the set of local cells which have at least one neighbor
	on another process.
	*/
	const boost::unordered_set<uint64_t>* get_cells_with_remote_neighbours(void) const
	{
		return &(this->cells_with_remote_neighbours);
	}

	/*!
	Returns a vector of local cells which have at least one neighbor on another process.
	*/
	std::vector<uint64_t> get_list_of_cells_with_remote_neighbours(void) const
	{
		std::vector<uint64_t> result(
			this->cells_with_remote_neighbours.begin(),
			this->cells_with_remote_neighbours.end()
		);
		return result;
	}

	/*!
	Returns a pointer to the set of remote cells which have at least one local neighbor.
	*/
	const boost::unordered_set<uint64_t>* get_remote_cells_with_local_neighbours(void) const
	{
		return &(this->remote_cells_with_local_neighbours);
	}

	/*!
	Returns a vector of remote cells which have at least one local neighbor.
	*/
	std::vector<uint64_t> get_list_of_remote_cells_with_local_neighbours(void) const
	{
		std::vector<uint64_t> result(
			this->remote_cells_with_local_neighbours.begin(),
			this->remote_cells_with_local_neighbours.end()
		);
		return result;
	}


	/*!
	Sets the weight of given local existing cell without children.

	Does nothing if above conditions are not met.
	Cell weights are given to Zoltan when balancing the load.
	Unset cell weights are assumed to be 1.

	User set cell weights are removed when balance_load is called.
	Children of refined cells inherit their parent's weight.
	Parents of unrefined cells do not inherit the moved cells' weights.
	*/
	void set_cell_weight(const uint64_t cell, const double weight)
	{
		if (this->cell_process.count(cell) == 0) {
			return;
		}

		if (this->cell_process.at(cell) != this->comm.rank()) {
			return;
		}

		if (cell != this->get_child(cell)) {
			return;
		}

		this->cell_weights[cell] = weight;
	}

	/*!
	Returns the weight of given local existing cell without children.

	Returns a quiet nan if above conditions are not met.
	Unset cell weights are assumed to be 1.
	*/
	double get_cell_weight(const uint64_t cell) const
	{
		if (this->cell_process.count(cell) == 0) {
			return std::numeric_limits<double>::quiet_NaN();
		}

		if (this->cell_process.at(cell) != this->comm.rank()) {
			return std::numeric_limits<double>::quiet_NaN();
		}

		if (cell != this->get_child(cell)) {
			return std::numeric_limits<double>::quiet_NaN();
		}

		if (this->cell_weights.count(cell) == 0) {
			return 1;
		} else {
			return this->cell_weights.at(cell);
		}
	}


	/*!
	Returns a pointer to the list of cells that will be added
	to this process after preparing to migrate or load balance cells.
	*/
	const boost::unordered_set<uint64_t>* get_balance_added_cells(void) const
	{
		return &(this->added_cells);
	}

	/*!
	Returns a pointer to the list of cells that will be removed
	from this process after preparing to migrate or load balance cells.
	*/
	const boost::unordered_set<uint64_t>* get_balance_removed_cells(void) const
	{
		return &(this->removed_cells);
	}


	/*!
	Returns the smallest existing cell at the given coordinate.

	Returns error_cell if the coordinate is outside of the grid or the cell is on another process.
	*/
	uint64_t get_existing_cell(const double x, const double y, const double z) const
	{
		if (x < this->get_x_start()
			|| x > this->get_x_end()
			|| y < this->get_y_start()
			|| y > this->get_y_end()
			|| z < this->get_z_start()
			|| z > this->get_z_end()
		) {
			return error_cell;
		}

		const Types<3>::indices_t indices = {
			this->get_x_index_of_coord(x),
			this->get_y_index_of_coord(y),
			this->get_z_index_of_coord(z)
		};

		return this->get_existing_cell(indices, 0, this->max_refinement_level);
	}



private:

	bool initialized;

	// size of the neighbour stencil of a cells in cells (of the same size as the cell itself)
	unsigned int neighbourhood_size;
	// the grid is distributed between these processes
	boost::mpi::communicator comm;

	// periodic[0] == true means that the grid wraps around in x direction
	bool periodic[3];

	// cells and their data on this process
	boost::unordered_map<uint64_t, UserData> cells;

	// cell on this process and its neighbours
	boost::unordered_map<uint64_t, std::vector<uint64_t> > neighbours;

	/*
	Offsets of cells that are considered as neighbours of a cell and
	offsets of cells that consider a cell as a neighbour
	*/
	std::vector<Types<3>::neighbourhood_item_t> neighbourhood_of, neighbourhood_to;

	/*!
	Cell on this process and those cells that aren't neighbours of this cell but whose neighbour this cell is.
	For example with a stencil size of 1 in the following grid:
\verbatim
|-----------|
|     |5 |6 |
|  1  |--|--|
|     |9 |10|
|-----------|
\endverbatim
	neighbours_to[6] = 1 because neighbours[6] = 5, 9, 10 while
	neighbours_to[5] is empty because neighbours[5] = 1, 6, 9, 10
	*/
	boost::unordered_map<uint64_t, std::vector<uint64_t> > neighbours_to;

	// on which process every cell in the grid is
	boost::unordered_map<uint64_t, int> cell_process;

	// cells on this process that have a neighbour on another process or are considered as a neighbour of a cell on another process
	boost::unordered_set<uint64_t> cells_with_remote_neighbours;

	// cells on other processes that have a neighbour on this process or are considered as a neighbour of a cell on this process
	boost::unordered_set<uint64_t> remote_cells_with_local_neighbours;

	// remote neighbours and their data, of cells on this process
	boost::unordered_map<uint64_t, UserData> remote_neighbours;

	#ifdef DCCRG_CELL_DATA_SIZE_FROM_USER
	boost::unordered_map<int, std::vector<MPI_Request> > send_requests, receive_requests;
	#else
	boost::unordered_map<int, std::vector<boost::mpi::request> > send_requests, receive_requests;
	#endif

	// cells whose data has to be received / sent by this process from the process as the key
	#ifdef DCCRG_SEND_SINGLE_CELLS
	// store cell, tag pairs so users can also send the data themselves easily
	boost::unordered_map<int, std::vector<std::pair<uint64_t, int> > > cells_to_send, cells_to_receive;
	#else
	boost::unordered_map<int, std::vector<uint64_t> > cells_to_send, cells_to_receive;
	#endif

	// cells added to / removed from this process by load balancing
	boost::unordered_set<uint64_t> added_cells, removed_cells;

	#ifndef DCCRG_SEND_SINGLE_CELLS
	#ifndef DCCRG_CELL_DATA_SIZE_FROM_USER
	// storage for cells' user data that awaits transfer to or from this process
	boost::unordered_map<int, std::vector<UserData> > incoming_data, outgoing_data;
	#endif
	#endif

	// cells to be refined / unrefined after a call to stop_refining()
	boost::unordered_set<uint64_t> cells_to_refine, cells_to_unrefine;

	// cells whose siblings shouldn't be unrefined
	boost::unordered_set<uint64_t> cells_not_to_unrefine;

	// stores user data of cells whose children were created while refining
	boost::unordered_map<uint64_t, UserData> refined_cell_data;
	// stores user data of cells that were removed while unrefining
	boost::unordered_map<uint64_t, UserData> unrefined_cell_data;

	// cell that should be kept on a particular process
	boost::unordered_map<uint64_t, int> pin_requests;
	// pin requests given since that last time load was balanced
	boost::unordered_map<uint64_t, int> new_pin_requests;

	// variables for load balancing using Zoltan
	Zoltan_Struct* zoltan;
	// number of processes per part in a hierarchy level (numbering starts from 0)
	std::vector<unsigned int> processes_per_part;
	// options for each level of hierarchial load balancing (numbering start from 0)
	std::vector<boost::unordered_map<std::string, std::string> > partitioning_options;
	// record whether Zoltan_LB_Partition is expected to fail (when the user selects NONE as the load balancing algorithm)
	bool no_load_balancing;
	// reserved options that the user cannot change
	boost::unordered_set<std::string> reserved_options;

	boost::unordered_map<uint64_t, double> cell_weights;


	/*!
	Moves cells between processes due to load balancing or user request.

	Recalculates neighbour lists, etc.
	Must be called simultaneously on all processes.
	*/
	void move_cells(void) {
		// TODO: get rid of added_cells and removed_cells and use cells_to_send and receive instead?

		this->cells_with_remote_neighbours.clear();
		this->remote_cells_with_local_neighbours.clear();
		this->remote_neighbours.clear();
		this->cells_to_refine.clear();
		this->refined_cell_data.clear();
		this->cells_to_unrefine.clear();
		this->unrefined_cell_data.clear();
		this->cells_not_to_unrefine.clear();

		/*
		Calculate where cells have migrated to update internal data structures
		Any cell can end up on any process and any neighbour of any cell can end up on yet another process
		*/

		// removed cells on all processes
		std::vector<uint64_t> temp_removed_cells(
			this->removed_cells.begin(),
			this->removed_cells.end()
		);
		std::sort(temp_removed_cells.begin(), temp_removed_cells.end());

		std::vector<std::vector<uint64_t> > all_removed_cells;
		all_gather(this->comm, temp_removed_cells, all_removed_cells);

		// created cells on all processes
		std::vector<uint64_t> temp_added_cells(
			this->added_cells.begin(),
			this->added_cells.end()
		);
		std::sort(temp_added_cells.begin(), temp_added_cells.end());

		std::vector<std::vector<uint64_t> > all_added_cells;
		all_gather(this->comm, temp_added_cells, all_added_cells);

		this->start_user_data_transfers(
		#ifdef DCCRG_SEND_SINGLE_CELLS
		this->cells
		#else
		#ifdef DCCRG_CELL_DATA_SIZE_FROM_USER
		this->cells
		#endif
		#endif
		);

		#ifdef DEBUG
		// check that there are no duplicate adds / removes
		boost::unordered_set<uint64_t> all_adds, all_removes;

		BOOST_FOREACH(std::vector<uint64_t> item, all_removed_cells) {
			BOOST_FOREACH(uint64_t removed_cell, item) {

				if (all_removes.count(removed_cell) > 0) {
					std::cerr << __FILE__ << ":" << __LINE__
						<< " Cell " << removed_cell
						<< " was already removed"
						<< std::endl;
					abort();
				}
				all_removes.insert(removed_cell);
			}
		}

		BOOST_FOREACH(std::vector<uint64_t> item, all_added_cells) {
			BOOST_FOREACH(uint64_t added_cell, item) {

				if (all_adds.count(added_cell) > 0) {
					std::cerr << __FILE__ << ":" << __LINE__
						<< " Cell " << added_cell
						<< " was already removed"
						<< std::endl;
					abort();
				}
				all_adds.insert(added_cell);
			}
		}

		// check that cells were removed by their process
		for (int cell_remover = 0; cell_remover < int(all_removed_cells.size()); cell_remover++) {

			BOOST_FOREACH(uint64_t removed_cell, all_removed_cells.at(cell_remover)) {

				if (this->cell_process.at(removed_cell) != cell_remover) {
					std::cerr << __FILE__ << ":" << __LINE__
						<< " Cell " << removed_cell
						<< " doesn't belong to process " << cell_remover
						<< std::endl;
					abort();
				}
			}
		}
		#endif

		// update cell to process mappings
		for (int cell_creator = 0; cell_creator < int(all_added_cells.size()); cell_creator++) {

			BOOST_FOREACH(uint64_t created_cell, all_added_cells.at(cell_creator)) {
				this->cell_process.at(created_cell) = cell_creator;
			}
		}

		#ifdef DEBUG
		if (!this->is_consistent()) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Grid is not consistent" << std::endl;
			exit(EXIT_FAILURE);
		}

		if (!this->pin_requests_succeeded()) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Pin requests didn't succeed" << std::endl;
			exit(EXIT_FAILURE);
		}
		#endif

		// create neighbour lists for cells without children that came to this process
		BOOST_FOREACH(uint64_t added_cell, this->added_cells) {

			if (added_cell != this->get_child(added_cell)) {
				continue;
			}

			this->neighbours[added_cell] = this->find_neighbours_of(added_cell);
			this->neighbours_to[added_cell] = this->find_neighbours_to(added_cell);
		}

		this->wait_user_data_transfer_receives(
		#ifndef DCCRG_SEND_SINGLE_CELLS
		#ifndef DCCRG_CELL_DATA_SIZE_FROM_USER
		this->cells
		#endif
		#endif
		);
		this->wait_user_data_transfer_sends();
		this->cells_to_send.clear();
		this->cells_to_receive.clear();

		// free user data and neighbour lists of cells removed from this process
		BOOST_FOREACH(uint64_t removed_cell, this->removed_cells) {
			this->cells.erase(removed_cell);
			this->neighbours.erase(removed_cell);
			this->neighbours_to.erase(removed_cell);
		}

		this->update_remote_neighbour_info();

		this->recalculate_neighbour_update_send_receive_lists();

		#ifdef DEBUG
		if (!this->is_consistent()) {
			std::cerr << __FILE__ << ":" << __LINE__ << " The grid is inconsistent" << std::endl;
			exit(EXIT_FAILURE);
		}

		if (!this->verify_neighbours()) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Neighbour lists are incorrect" << std::endl;
			exit(EXIT_FAILURE);
		}

		if (!this->verify_remote_neighbour_info()) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Remote neighbour info is not consistent" << std::endl;
			exit(EXIT_FAILURE);
		}

		if (!this->verify_user_data()) {
			std::cerr << __FILE__ << ":" << __LINE__ << " virhe" << std::endl;
			exit(EXIT_FAILURE);
		}
		#endif
	}

	/*!
	Prepares to move cells between processes with move_cells.

	Sends user data of cells between processes once before move_cells so that
	the correct mpi datatype can be constructed when actually moving cells.

	Must be called simultaneously on all processes.
	move_cells must be the next dccrg function to be called after this one.
	*/
	void prepare_to_move_cells(void) {
		#ifdef DEBUG
		if (!this->verify_remote_neighbour_info()) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Remote neighbour info is not consistent" << std::endl;
			exit(EXIT_FAILURE);
		}

		if (!this->verify_user_data()) {
			std::cerr << __FILE__ << ":" << __LINE__ << " virhe" << std::endl;
			exit(EXIT_FAILURE);
		}
		#endif

		this->cells_with_remote_neighbours.clear();
		this->remote_cells_with_local_neighbours.clear();
		this->remote_neighbours.clear();
		this->cells_to_refine.clear();
		this->refined_cell_data.clear();
		this->cells_to_unrefine.clear();
		this->unrefined_cell_data.clear();

		this->start_user_data_transfers(
		#ifdef DCCRG_SEND_SINGLE_CELLS
		this->cells
		#else
		#ifdef DCCRG_CELL_DATA_SIZE_FROM_USER
		this->cells
		#endif
		#endif
		);

		#ifdef DEBUG
		// check that there are no duplicate adds / removes
		// removed cells on all processes
		std::vector<uint64_t> temp_removed_cells(
			this->removed_cells.begin(),
			this->removed_cells.end()
		);
		std::sort(temp_removed_cells.begin(), temp_removed_cells.end());

		std::vector<std::vector<uint64_t> > all_removed_cells;
		all_gather(this->comm, temp_removed_cells, all_removed_cells);

		// created cells on all processes
		std::vector<uint64_t> temp_added_cells(
			this->added_cells.begin(),
			this->added_cells.end()
		);
		std::sort(temp_added_cells.begin(), temp_added_cells.end());

		std::vector<std::vector<uint64_t> > all_added_cells;
		all_gather(this->comm, temp_added_cells, all_added_cells);

		boost::unordered_set<uint64_t> all_adds, all_removes;

		BOOST_FOREACH(std::vector<uint64_t> item, all_removed_cells) {
			BOOST_FOREACH(uint64_t removed_cell, item) {

				if (all_removes.count(removed_cell) > 0) {
					std::cerr << __FILE__ << ":" << __LINE__
						<< " Cell " << removed_cell
						<< " was already removed"
						<< std::endl;
					abort();
				}
				all_removes.insert(removed_cell);
			}
		}

		BOOST_FOREACH(std::vector<uint64_t> item, all_added_cells) {
			BOOST_FOREACH(uint64_t added_cell, item) {

				if (all_adds.count(added_cell) > 0) {
					std::cerr << __FILE__ << ":" << __LINE__
						<< " Cell " << added_cell
						<< " was already removed"
						<< std::endl;
					abort();
				}
				all_adds.insert(added_cell);
			}
		}

		// check that cells were removed by their process
		for (int cell_remover = 0; cell_remover < int(all_removed_cells.size()); cell_remover++) {

			BOOST_FOREACH(uint64_t removed_cell, all_removed_cells.at(cell_remover)) {

				if (this->cell_process.at(removed_cell) != cell_remover) {
					std::cerr << __FILE__ << ":" << __LINE__
						<< " Cell " << removed_cell
						<< " doesn't belong to process " << cell_remover
						<< std::endl;
					abort();
				}
			}
		}
		#endif

		this->wait_user_data_transfer_receives(
		#ifndef DCCRG_SEND_SINGLE_CELLS
		#ifndef DCCRG_CELL_DATA_SIZE_FROM_USER
		this->cells
		#endif
		#endif
		);
		this->wait_user_data_transfer_sends();
	}


	/*!
	Updates user pin requests globally based on new_pin_requests.

	Must be called simultaneously on all processes.
	*/
	void update_pin_requests(void)
	{
		std::vector<uint64_t> new_pinned_cells, new_pinned_processes;

		new_pinned_cells.reserve(this->new_pin_requests.size());
		new_pinned_processes.reserve(this->new_pin_requests.size());
		for (boost::unordered_map<uint64_t, int>::const_iterator
			item = this->new_pin_requests.begin();
			item != this->new_pin_requests.end();
			item++
		) {
			new_pinned_cells.push_back(item->first);
			new_pinned_processes.push_back(item->second);
		}

		std::vector<std::vector<uint64_t> > all_new_pinned_cells, all_new_pinned_processes;
		all_gather(this->comm, new_pinned_cells, all_new_pinned_cells);
		all_gather(this->comm, new_pinned_processes, all_new_pinned_processes);

		for (int process = 0; process < int(all_new_pinned_cells.size()); process++) {
			for (unsigned int i = 0; i < all_new_pinned_cells.at(process).size(); i++) {

				const int requested_process = all_new_pinned_processes[process][i];

				if (requested_process == -1) {
					this->pin_requests.erase(all_new_pinned_cells[process][i]);
				} else {
					this->pin_requests[all_new_pinned_cells[process][i]] = requested_process;
				}

				#ifdef DEBUG
				if (this->cell_process.at(all_new_pinned_cells[process][i]) != process) {
					std::cerr << __FILE__ << ":" << __LINE__ << " Process " << process << " tried pin cell " << all_new_pinned_cells[process][i] << std::endl;
					exit(EXIT_FAILURE);
				}
				#endif
			}
		}

		this->new_pin_requests.clear();
	}


	/*!
	Repartitions cells across processes based on user requests and
	Zoltan if use_zoltan is true.

	Updates send & receive lists.
	*/
	void make_new_partition(const bool use_zoltan)
	{
		this->cell_weights.clear();

		this->update_pin_requests();

		int partition_changed, global_id_size, local_id_size, number_to_receive, number_to_send;
		ZOLTAN_ID_PTR global_ids_to_receive, local_ids_to_receive, global_ids_to_send, local_ids_to_send;
		int *sender_processes, *receiver_processes;

		if (use_zoltan && Zoltan_LB_Balance(
			this->zoltan,
			&partition_changed,
			&global_id_size,
			&local_id_size,
			&number_to_receive,
			&global_ids_to_receive,
			&local_ids_to_receive,
			&sender_processes,
			&number_to_send,
			&global_ids_to_send,
			&local_ids_to_send,
			&receiver_processes
			) != ZOLTAN_OK
		) {
			if (!this->no_load_balancing) {
				if (this->comm.rank() == 0) {
					std::cerr << "Zoltan_LB_Partition failed" << std::endl;
				}
				Zoltan_Destroy(&this->zoltan);
				// TODO: throw an exception instead
				abort();
			}

			#ifdef DEBUG
			// check that processes have the cells they're supposed to send
			for (int i = 0; i < number_to_send; i++) {
				if (this->cells.count(global_ids_to_send[i]) == 0) {
					std::cerr << __FILE__ << ":" << __LINE__ << " Cannot send cell " << global_ids_to_send[i] << " to process " << receiver_processes[i] << std::endl;
					abort();
				}
			}

			// check that cells to be received are on the sending process
			for (int i = 0; i < number_to_receive; i++) {
				if (this->cell_process.at(global_ids_to_receive[i]) != sender_processes[i]) {
					std::cerr << __FILE__ << ":" << __LINE__ << " Cannot receive cell " << global_ids_to_receive[i] << " from process " << sender_processes[i] << std::endl;
					abort();
				}
			}
			#endif
		}

		this->cells_to_receive.clear();
		this->cells_to_send.clear();

		/*
		Processes and the cells for which data has to be received by this process
		*/

		// migration from user
		for (boost::unordered_map<uint64_t, int>::const_iterator
			pin_request = this->pin_requests.begin();
			pin_request != this->pin_requests.end();
			pin_request++
		) {
			const int current_process_of_cell = this->cell_process.at(pin_request->first);

			if (pin_request->second == this->comm.rank()
			&& current_process_of_cell != this->comm.rank()) {
				this->cells_to_receive[current_process_of_cell].push_back(
					#ifdef DCCRG_SEND_SINGLE_CELLS
					std::make_pair(pin_request->first, -1)
					#else
					pin_request->first
					#endif
				);
				this->added_cells.insert(pin_request->first);
			}
		}

		// migration from Zoltan
		if (use_zoltan) {
			for (int i = 0; i < number_to_receive; i++) {

				// don't send / receive from self
				if (sender_processes[i] == this->comm.rank()) {
					continue;
				}

				// skip user-migrated cells
				if (this->pin_requests.count(global_ids_to_receive[i]) > 0) {
					continue;
				}

				this->cells_to_receive[sender_processes[i]].push_back(
					#ifdef DCCRG_SEND_SINGLE_CELLS
					std::make_pair(global_ids_to_receive[i], -1)
					#else
					global_ids_to_receive[i]
					#endif
				);

				#ifdef DEBUG
				if (added_cells.count(global_ids_to_receive[i]) > 0) {
					std::cerr << __FILE__ << ":" << __LINE__
						<< " Cell " << global_ids_to_receive[i]
						<< " has already been received from process " << this->comm.rank()
						<< std::endl;
					abort();
				}
				#endif

				this->added_cells.insert(global_ids_to_receive[i]);
			}
		}

		// receive cells in known order and add message tags
		for (
			#ifdef DCCRG_SEND_SINGLE_CELLS
			boost::unordered_map<int, std::vector<std::pair<uint64_t, int> > >::iterator
			#else
			boost::unordered_map<int, std::vector<uint64_t> >::iterator
			#endif
			sender = this->cells_to_receive.begin();
			sender != this->cells_to_receive.end();
			sender++
		) {
			std::sort(sender->second.begin(), sender->second.end());
			#ifdef DCCRG_SEND_SINGLE_CELLS
			// TODO: check that message tags don't overflow
			for (unsigned int i = 0; i < sender->second.size(); i++) {
				sender->second[i].second = i + 1;
			}
			#endif
		}


		/*
		Processes and the cells for which data has to be sent by this process
		*/

		// migration from user
		for (boost::unordered_map<uint64_t, int>::const_iterator
			pin_request = this->pin_requests.begin();
			pin_request != this->pin_requests.end();
			pin_request++
		) {
			const int current_process_of_cell = this->cell_process.at(pin_request->first);
			const int destination_process = pin_request->second;

			if (destination_process != this->comm.rank()
			&& current_process_of_cell == this->comm.rank()) {
				this->cells_to_send[destination_process].push_back(
					#ifdef DCCRG_SEND_SINGLE_CELLS
					std::make_pair(pin_request->first, -1)
					#else
					pin_request->first
					#endif
				);
				this->removed_cells.insert(pin_request->first);
			}
		}

		// migration from Zoltan
		if (use_zoltan) {
			for (int i = 0; i < number_to_send; i++) {

				// don't send / receive from self
				if (receiver_processes[i] == this->comm.rank()) {
					continue;
				}

				// skip user-migrated cells
				if (this->pin_requests.count(global_ids_to_send[i]) > 0) {
					continue;
				}

				this->cells_to_send[receiver_processes[i]].push_back(
					#ifdef DCCRG_SEND_SINGLE_CELLS
					std::make_pair(global_ids_to_send[i], -1)
					#else
					global_ids_to_send[i]
					#endif
				);

				#ifdef DEBUG
				if (removed_cells.count(global_ids_to_send[i]) > 0) {
					std::cerr << __FILE__ << ":" << __LINE__
						<< " Cell " << global_ids_to_send[i]
						<< " has already been sent from process " << this->comm.rank()
						<< std::endl;
					abort();
				}
				#endif

				this->removed_cells.insert(global_ids_to_send[i]);
			}

			Zoltan_LB_Free_Data(
				&global_ids_to_receive,
				&local_ids_to_receive,
				&sender_processes,
				&global_ids_to_send,
				&local_ids_to_send,
				&receiver_processes
			);
		}

		// send cells in known order and add message tags
		for (
			#ifdef DCCRG_SEND_SINGLE_CELLS
			boost::unordered_map<int, std::vector<std::pair<uint64_t, int> > >::iterator
			#else
			boost::unordered_map<int, std::vector<uint64_t> >::iterator
			#endif
			receiver = this->cells_to_send.begin();
			receiver != this->cells_to_send.end();
			receiver++
		) {
			std::sort(receiver->second.begin(), receiver->second.end());
			#ifdef DCCRG_SEND_SINGLE_CELLS
			// TODO: check that message tags don't overflow
			for (unsigned int i = 0; i < receiver->second.size(); i++) {
				receiver->second[i].second = i + 1;
			}
			#endif
		}
	}


	/*!
	Calculates what to send and where during a remote neighbour data update.

	Assumes up-to-date neighbour lists, clears previous send / receive lists.
	*/
	void recalculate_neighbour_update_send_receive_lists(void)
	{
		// clear previous lists
		this->cells_to_send.clear();
		this->cells_to_receive.clear();

		// only send a cell to a process once
		boost::unordered_map<int, boost::unordered_set<uint64_t> > unique_cells_to_send, unique_cells_to_receive;

		// calculate new lists for neighbour data updates
		BOOST_FOREACH(uint64_t cell, this->cells_with_remote_neighbours) {

			#ifdef DEBUG
			if (cell != this->get_child(cell)) {
				std::cerr << __FILE__ << ":" << __LINE__
					<< " Cell " << cell << " has children"
					<< std::endl;
				abort();
			}
			#endif

			int current_process = this->comm.rank();

			// data must be received from neighbours_of
			BOOST_FOREACH(uint64_t neighbour, this->neighbours.at(cell)) {

				if (neighbour == 0) {
					continue;
				}

				if (this->cell_process.at(neighbour) != current_process) {
					unique_cells_to_receive[this->cell_process.at(neighbour)].insert(neighbour);
				}
			}

			// data must be sent to neighbours_to
			BOOST_FOREACH(uint64_t neighbour, this->neighbours_to.at(cell)) {

				if (neighbour == 0) {
					continue;
				}

				if (this->cell_process.at(neighbour) != current_process) {
					unique_cells_to_send[this->cell_process.at(neighbour)].insert(cell);
				}
			}
		}

		// populate final send list data structures and sort them
		for (boost::unordered_map<int, boost::unordered_set<uint64_t> >::const_iterator
			receiver = unique_cells_to_send.begin();
			receiver != unique_cells_to_send.end();
			receiver++
		) {
			this->cells_to_send[receiver->first].reserve(receiver->second.size());

			BOOST_FOREACH(uint64_t cell, receiver->second) {
				this->cells_to_send[receiver->first].push_back(
					#ifdef DCCRG_SEND_SINGLE_CELLS
					std::make_pair(cell, -1)
					#else
					cell
					#endif
				);
			}

			sort(
				this->cells_to_send[receiver->first].begin(),
				this->cells_to_send[receiver->first].end()
			);

			#ifdef DCCRG_SEND_SINGLE_CELLS
			// sequential tags for messages: 1, 2, ...
			for (unsigned int i = 0; i < this->cells_to_send[receiver->first].size(); i++) {
				this->cells_to_send[receiver->first][i].second = i + 1;
			}
			#endif
		}

		// populate final receive list data structures and sort them
		for (boost::unordered_map<int, boost::unordered_set<uint64_t> >::const_iterator
			sender = unique_cells_to_receive.begin();
			sender != unique_cells_to_receive.end();
			sender++
		) {
			this->cells_to_receive[sender->first].reserve(sender->second.size());

			BOOST_FOREACH(uint64_t cell, sender->second) {

				this->cells_to_receive[sender->first].push_back(
					#ifdef DCCRG_SEND_SINGLE_CELLS
					std::make_pair(cell, -1)
					#else
					cell
					#endif
				);
			}

			sort(
				this->cells_to_receive[sender->first].begin(),
				this->cells_to_receive[sender->first].end()
			);

			#ifdef DCCRG_SEND_SINGLE_CELLS
			// sequential tags for messages: 1, 2, ...
			for (unsigned int i = 0; i < this->cells_to_receive[sender->first].size(); i++) {
				this->cells_to_receive[sender->first][i].second = i + 1;
			}
			#endif
		}
	}


	/*!
	Updates neighbour and neighbour_to lists around given cell's neighbourhood.

	Does nothing in the following cases:
		-given cell doesn't exist in the grid
		-given cell has children
	Assumes that the refinement level difference between given cell and its neighbourhood is no larger than 1.
	*/
	void update_neighbours(const uint64_t cell)
	{
		if (this->cell_process.at(cell) != this->comm.rank()) {
			return;
		}

		if (cell != this->get_child(cell)) {
			return;
		}

		// get the neighbors_of of given cell
		this->neighbours.at(cell) = this->find_neighbours_of(cell);
		this->neighbours_to.at(cell) = this->find_neighbours_to(cell, this->neighbours.at(cell));

		#ifdef DEBUG
		if (!this->verify_neighbours(cell)) {
			std::cerr << __FILE__ << ":" << __LINE__
				<< " Neighbour update failed for cell " << cell
				<< " (child of " << this->get_parent(cell) << ")"
				<< std::endl;
			abort();
		}
		#endif
	}


	/*!
	Updates the remote neighbour info of given cell on this process without children.

	Uses current neighbour lists.
	Does nothing if given cell doesn't exist on this process or has children
	*/
	void update_remote_neighbour_info(const uint64_t cell)
	{
		if (this->cells.count(cell) == 0) {
			return;
		}

		if (cell != this->get_child(cell)) {
			return;
		}

		// TODO: also update remote_cells_with_local_neighbours
		this->cells_with_remote_neighbours.erase(cell);

		#ifdef DEBUG
		if (this->neighbours.count(cell) == 0) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Neighbour list for cell " << cell << " doesn't exist" << std::endl;
			abort();
		}

		if (this->neighbours_to.count(cell) == 0) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Neighbours_to list for cell " << cell << " doesn't exist" << std::endl;
			abort();
		}
		#endif

		// neighbours of given cell
		BOOST_FOREACH(uint64_t neighbour, this->neighbours.at(cell)) {

			if (neighbour == 0) {
				continue;
			}

			if (this->cell_process.at(neighbour) != this->comm.rank()) {
				this->cells_with_remote_neighbours.insert(cell);
				this->remote_cells_with_local_neighbours.insert(neighbour);
			}
		}
		// cells with given cell as neighbour
		BOOST_FOREACH(uint64_t neighbour_to, this->neighbours_to.at(cell)) {

			if (this->cell_process.at(neighbour_to) != this->comm.rank()) {
				this->cells_with_remote_neighbours.insert(cell);
				this->remote_cells_with_local_neighbours.insert(neighbour_to);
			}
		}

		#ifdef DEBUG
		if (!this->verify_remote_neighbour_info(cell)) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Remote neighbour info for cell " << cell << " is not consistent" << std::endl;
			abort();
		}
		#endif
	}


	/*!
	Updates the remote neighbour info of all cells on this process without children.

	Uses current neighbour lists.
	*/
	void update_remote_neighbour_info(void)
	{
		// TODO this probably can't be optimized without storing neighbour lists also for remote neighbours
		this->cells_with_remote_neighbours.clear();
		this->remote_cells_with_local_neighbours.clear();

		BOOST_FOREACH(cell_and_data_pair_t item, this->cells) {

			if (item.first != this->get_child(item.first)) {
				continue;
			}

			this->update_remote_neighbour_info(item.first);

			#ifdef DEBUG
			if (!this->verify_remote_neighbour_info(item.first)) {
				std::cerr << __FILE__ << ":" << __LINE__
					<< " Remote neighbour info for cell " << item.first
					<< " is not consistent"
					<< std::endl;
				abort();
			}
			#endif
		}

		#ifdef DEBUG
		if (!this->verify_remote_neighbour_info()) {
			std::cerr << __FILE__ << ":" << __LINE__
				<< " Remote neighbour info is not consistent"
				<< std::endl;
			abort();
		}
		#endif
	}


	/*!
	Returns true if cell1 considers cell2 as a neighbour, even if neither of them exists
	*/
	bool is_neighbour(const uint64_t cell1, const uint64_t cell2) const
	{
		#ifdef DEBUG
		if (cell1 == 0) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Invalid cell1 given." << std::endl;
			abort();
		}

		if (cell1 > this->last_cell) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Impossible cell1 given." << std::endl;
			abort();
		}

		if (cell2 == 0) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Invalid cell2 given." << std::endl;
			abort();
		}

		if (cell2 > this->last_cell) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Impossible cell2 given." << std::endl;
			abort();
		}
		#endif

		const Types<3>::indices_t indices1 = this->get_indices(cell1);
		const Types<3>::indices_t indices2 = this->get_indices(cell2);
		const uint64_t cell1_size = this->get_cell_size_in_indices(cell1);
		const uint64_t cell2_size = this->get_cell_size_in_indices(cell2);

		// distance in indices between given cells
		Types<3>::indices_t distance = {0, 0, 0};

		const uint64_t grid_length[3] = {
			this->get_x_length() * (uint64_t(1) << this->max_refinement_level),
			this->get_y_length() * (uint64_t(1) << this->max_refinement_level),
			this->get_z_length() * (uint64_t(1) << this->max_refinement_level)
		};

		uint64_t max_distance = 0;

		for (unsigned int i = 0; i < 3; i++) {
			if (indices1[i] <= indices2[i]) {
				if (indices2[i] <= indices1[i] + cell1_size) {
					distance[i] = 0;
				} else {
					distance[i] = indices2[i] - (indices1[i] + cell1_size);
				}

				if (this->periodic[i]) {
					const uint64_t distance_to_end = grid_length[i] - (indices2[i] + cell2_size);
					distance[i] = std::min(distance[i], indices1[i] + distance_to_end);
				}
			} else {
				if (indices1[i] <= indices2[i] + cell2_size) {
					distance[i] = 0;
				} else {
					distance[i] = indices1[i] - (indices2[i] + cell2_size);
				}

				if (this->periodic[i]) {
					const uint64_t distance_to_end = grid_length[i] - (indices1[i] + cell1_size);
					distance[i] = std::min(distance[i], indices2[i] + distance_to_end);
				}
			}

			max_distance = std::max(max_distance, distance[i]);
		}

		if (this->neighbourhood_size == 0) {
			if (max_distance < cell1_size
			&& this->overlapping_indices(cell1, cell2) >= 2) {
				return true;
			// diagonal cell isn't a neighbour
			} else {
				return false;
			}
		}

		if (max_distance < this->neighbourhood_size * cell1_size) {
			return true;
		} else {
			return false;
		}
	}


	/*!
	Given a cell that exists and has children returns one of the children
	Returns the given cell if it doesn't have children or 0 if the cell doesn't exist
	*/
	uint64_t get_child(const uint64_t cell) const
	{
		if (this->cell_process.count(cell) == 0) {
			return 0;
		}

		const int refinement_level = this->get_refinement_level(cell);

		// given cell cannot have children
		if (refinement_level == this->max_refinement_level) {
			return cell;
		}

		const uint64_t child = this->get_cell_from_indices(
			this->get_indices(cell),
			refinement_level + 1
		);

		if (this->cell_process.count(child) > 0) {
			return child;
		} else {
			return cell;
		}
	}


	/*!
	Adds new cells to cells_to_refine in order to enforce maximum refinement level difference of one between neighbours (also across processes).

	After this function cells_to_refine will contain the refines of all processes.
	*/
	void induce_refines(void)
	{
		std::vector<uint64_t> new_refines(this->cells_to_refine.begin(), this->cells_to_refine.end());
		while (all_reduce(this->comm, new_refines.size(), std::plus<uint64_t>()) > 0) {

			std::vector<std::vector<uint64_t> > all_new_refines;
			all_gather(this->comm, new_refines, all_new_refines);
			new_refines.clear();

			boost::unordered_set<uint64_t> unique_induced_refines;

			// induced refines on this process
			BOOST_FOREACH(uint64_t refined, all_new_refines.at(this->comm.rank())) {

				// refine local neighbours that are too large
				BOOST_FOREACH(uint64_t neighbour, this->neighbours.at(refined)) {

					if (neighbour == 0) {
						continue;
					}

					#ifdef DEBUG
					if (this->cell_process.count(neighbour) == 0) {
						std::cerr << "Process " << this->comm.rank()
							<< ": Cell " << refined
							<< " had a non-existing neighbour in neighbour list: " << neighbour
							<< std::endl;
					}
					#endif

					if (this->cell_process.at(neighbour) != this->comm.rank()) {
						continue;
					}

					if (this->get_refinement_level(neighbour) < this->get_refinement_level(refined)) {
						if (this->cells_to_refine.count(neighbour) == 0) {
							unique_induced_refines.insert(neighbour);
						}
					}
				}

				BOOST_FOREACH(uint64_t neighbour_to, this->neighbours_to.at(refined)) {

					if (neighbour_to == 0) {
						continue;
					}

					#ifdef DEBUG
					if (this->cell_process.count(neighbour_to) == 0) {
						std::cerr << "Process " << this->comm.rank()
							<< ": Cell " << refined
							<< " had a non-existing neighbour in neighbour list: " << neighbour_to
							<< std::endl;
					}
					#endif

					if (this->cell_process.at(neighbour_to) != this->comm.rank()) {
						continue;
					}

					if (this->get_refinement_level(neighbour_to) < this->get_refinement_level(refined)) {
						if (this->cells_to_refine.count(neighbour_to) == 0) {
							unique_induced_refines.insert(neighbour_to);
						}
					}
				}
			}

			// refines induced here by other processes
			for (int process = 0; process < this->comm.size(); process++) {

				if (process == this->comm.rank()) {
					continue;
				}

				BOOST_FOREACH(uint64_t refined, all_new_refines.at(process)) {

					if (this->remote_cells_with_local_neighbours.count(refined) == 0) {
						continue;
					}

					// refine all local cells that are too large and neighbouring the refined cell
					/*
					TODO: probably faster to search for local neighbours of refined
					cell, even faster would be to also store neighbours lists of
					remote cells with local neighbours
					*/
					BOOST_FOREACH(uint64_t local, this->cells_with_remote_neighbours) {

						if (this->is_neighbour(local, refined)
						&& this->get_refinement_level(local) < this->get_refinement_level(refined)
						&& this->cells_to_refine.count(local) == 0) {
							unique_induced_refines.insert(local);
						}
					}
				}
			}
			all_new_refines.clear();

			new_refines.insert(new_refines.end(), unique_induced_refines.begin(), unique_induced_refines.end());
			this->cells_to_refine.insert(unique_induced_refines.begin(), unique_induced_refines.end());
			unique_induced_refines.clear();
		}

		// reduce future global communication by adding refines from all processes to cells_to_refine
		std::vector<uint64_t> refines(this->cells_to_refine.begin(), this->cells_to_refine.end());
		std::vector<std::vector<uint64_t> > all_refines;
		all_gather(this->comm, refines, all_refines);

		for (int process = 0; process < this->comm.size(); process++) {
			this->cells_to_refine.insert(all_refines[process].begin(), all_refines[process].end());
		}

		#ifdef DEBUG
		// check that all required refines have been induced
		BOOST_FOREACH(uint64_t refined, this->cells_to_refine) {

			// neighbours_of
			std::vector<uint64_t> neighbours_of = this->find_neighbours_of(refined);

			BOOST_FOREACH(uint64_t neighbour_of, neighbours_of) {

				if (neighbour_of == 0) {
					continue;
				}

				if (this->get_refinement_level(neighbour_of) < this->get_refinement_level(refined)
				&& this->cells_to_refine.count(neighbour_of) == 0) {
					std::cerr << __FILE__ << ":" << __LINE__
						<< " Neighbour (" << neighbour_of
						<< ") of cell that will be refined (" << refined
						<< ", ref lvl " << this->get_refinement_level(refined)
						<< ") has too small refinement level: " << this->get_refinement_level(neighbour_of)
						<< std::endl;
					abort();
				}
			}

			// neighbours_to
			std::vector<uint64_t> neighbours_to = this->find_neighbours_to(refined);
			BOOST_FOREACH(uint64_t neighbour_to, neighbours_to) {

				if (neighbour_to == 0) {
					continue;
				}

				if (this->get_refinement_level(neighbour_to) < this->get_refinement_level(refined)
				&& this->cells_to_refine.count(neighbour_to) == 0) {
					std::cerr << __FILE__ << ":" << __LINE__
						<< " Neighbour (" << neighbour_to
						<< ") of cell that will be refined (" << refined
						<< ", ref lvl " << this->get_refinement_level(refined)
						<< ") has too small refinement level: " << this->get_refinement_level(neighbour_to)
						<< std::endl;
					abort();
				}
			}
		}

		if (!this->is_consistent()) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Grid isn't consistent" << std::endl;
			abort();
		}
		#endif
	}


	/*!
	Sends the numbers in s to all other processes and adds the numbers sent by all others to s.
	*/
	void all_to_all_set(boost::unordered_set<uint64_t>& s)
	{
		std::vector<uint64_t> local_s(s.begin(), s.end());

		std::vector<std::vector<uint64_t> > all_s;
		all_gather(this->comm, local_s, all_s);

		BOOST_FOREACH(std::vector<uint64_t> i, all_s) {
			BOOST_FOREACH(uint64_t cell, i) {
				s.insert(cell);
			}
		}
	}


	/*!
	Removes cells from cells_to_unrefine in order to enforce maximum refinement level difference of one between neighbours (also across processes).

	cells_to_refine and cells_not_to_unrefine must be identical between processes.
	After this function cells_to_unrefine will contain the unrefines of all processes.
	*/
	void override_unrefines(void)
	{
		// unrefines that were not overridden by refines or too small neighbours
		boost::unordered_set<uint64_t> final_unrefines;

		BOOST_FOREACH(uint64_t unrefined, this->cells_to_unrefine) {

			#ifdef DEBUG
			if (unrefined == 0) {
				std::cerr << __FILE__ << ":" << __LINE__ << " Invalid cell to unrefine" << std::endl;
				abort();
			}
			#endif

			bool can_unrefine = true;

			// any sibling being refined will override this unrefine
			const std::vector<uint64_t> siblings = this->get_all_children(this->get_parent(unrefined));
			BOOST_FOREACH(uint64_t sibling, siblings) {
				if (this->cells_to_refine.count(sibling) > 0
				// don't unrefine if requested not to
				|| this->cells_not_to_unrefine.count(sibling) > 0) {
					can_unrefine = false;
					break;
				}
			}

			// TODO improve performance by first using local neighbour lists to override unrefines
			std::vector<uint64_t> neighbours_of_parent = this->find_neighbours_of(this->get_parent(unrefined));
			BOOST_FOREACH(uint64_t neighbour_of_parent, neighbours_of_parent) {

				if (this->get_refinement_level(neighbour_of_parent) < this->get_refinement_level(unrefined)) {
					can_unrefine = false;
					break;
				}

				if (this->cells_to_refine.count(neighbour_of_parent) > 0
				&& this->get_refinement_level(neighbour_of_parent) <= this->get_refinement_level(unrefined)) {
					can_unrefine = false;
					break;
				}
			}

			if (can_unrefine) {
				final_unrefines.insert(unrefined);
			}
		}
		this->cells_to_unrefine.clear();

		// reduce future global communication by adding unrefines from all processes to cells_to_unrefine
		std::vector<uint64_t> unrefines(final_unrefines.begin(), final_unrefines.end());
		std::vector<std::vector<uint64_t> > all_unrefines;
		all_gather(this->comm, unrefines, all_unrefines);

		for (int process = 0; process < this->comm.size(); process++) {
			this->cells_to_unrefine.insert(all_unrefines[process].begin(), all_unrefines[process].end());
		}

		#ifdef DEBUG
		if (!this->is_consistent()) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Grid isn't consistent" << std::endl;
			exit(EXIT_FAILURE);
		}
		#endif
	}


	/*!
	Adds refined cells to the grid, removes unrefined cells from the grid.

	cells_to_refine and cells_to_unrefine must contain the cells to refine/unrefine of all processes.
	Returns new cells created on this process by refinement.
	Moves unrefined cell data to the process of their parent.
	*/
	std::vector<uint64_t> execute_refines(void)
	{
		#ifdef DEBUG
		if (!this->verify_remote_neighbour_info()) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Remote neighbour info is not consistent" << std::endl;
			exit(EXIT_FAILURE);
		}

		if (!this->verify_user_data()) {
			std::cerr << __FILE__ << ":" << __LINE__ << " User data is inconsistent" << std::endl;
			exit(EXIT_FAILURE);
		}
		#endif

		std::vector<uint64_t> new_cells;

		this->remote_neighbours.clear();
		this->cells_to_send.clear();
		this->cells_to_receive.clear();
		this->refined_cell_data.clear();
		this->unrefined_cell_data.clear();
		#ifndef DCCRG_SEND_SINGLE_CELLS
		#ifndef DCCRG_CELL_DATA_SIZE_FROM_USER
		this->incoming_data.clear();
		this->outgoing_data.clear();
		#endif
		#endif

		#ifdef DEBUG
		// check that cells_to_refine is identical between processes
		std::vector<uint64_t> ordered_cells_to_refine(this->cells_to_refine.begin(), this->cells_to_refine.end());
		sort(ordered_cells_to_refine.begin(), ordered_cells_to_refine.end());

		std::vector<std::vector<uint64_t> > all_ordered_cells_to_refine;
		all_gather(this->comm, ordered_cells_to_refine, all_ordered_cells_to_refine);

		for (int process = 0; process < this->comm.size(); process++) {
			if (!std::equal(all_ordered_cells_to_refine[process].begin(), all_ordered_cells_to_refine[process].end(), all_ordered_cells_to_refine[0].begin())) {
				std::cerr << __FILE__ << ":" << __LINE__ << " cells_to_refine differ between processes 0 and " << process << std::endl;
				exit(EXIT_FAILURE);
			}
		}

		// check that cells_to_unrefine is identical between processes
		std::vector<uint64_t> ordered_cells_to_unrefine(this->cells_to_unrefine.begin(), this->cells_to_unrefine.end());
		sort(ordered_cells_to_unrefine.begin(), ordered_cells_to_unrefine.end());

		std::vector<std::vector<uint64_t> > all_ordered_cells_to_unrefine;
		all_gather(this->comm, ordered_cells_to_unrefine, all_ordered_cells_to_unrefine);

		for (int process = 0; process < this->comm.size(); process++) {
			if (!std::equal(all_ordered_cells_to_unrefine[process].begin(), all_ordered_cells_to_unrefine[process].end(), all_ordered_cells_to_unrefine[0].begin())) {
				std::cerr << __FILE__ << ":" << __LINE__ << " cells_to_unrefine differ between processes 0 and " << process << std::endl;
				exit(EXIT_FAILURE);
			}
		}
		#endif

		// cells whose neighbour lists have to be updated afterwards
		boost::unordered_set<uint64_t> update_neighbours;

		// a separate neighbourhood update function has to be used for cells whose children were removed by unrefining
		boost::unordered_set<uint64_t> update_neighbours_unrefined;

		// refines
		BOOST_FOREACH(uint64_t refined, this->cells_to_refine) {

			#ifdef DEBUG
			if (this->cell_process.count(refined) == 0) {
				std::cerr << __FILE__ << ":" << __LINE__
					<< " Cell " << refined
					<< " doesn't exist"
					<< std::endl;
				abort();
			}

			if (this->comm.rank() == this->cell_process.at(refined)
			&& this->cells.count(refined) == 0) {
				std::cerr << __FILE__ << ":" << __LINE__
					<< " Data for cell " << refined
					<< " doesn't exist"
					<< std::endl;
				abort();
			}


			if (this->neighbours.count(refined) == 0) {
				std::cerr << __FILE__ << ":" << __LINE__
					<< " Neighbor list for cell " << refined
					<< " doesn't exist"
					<< std::endl;
				abort();
			}

			if (this->neighbours_to.count(refined) == 0) {
				std::cerr << __FILE__ << ":" << __LINE__
					<< " Neighbor_to list for cell " << refined
					<< " doesn't exist"
					<< std::endl;
				abort();
			}
			#endif

			const int process_of_refined = this->cell_process.at(refined);

			// move user data of refined cells into refined_cell_data
			if (this->comm.rank() == process_of_refined) {
				// TODO: move data instead of copying, using boost::move or c++0x move?
				this->refined_cell_data[refined] = this->cells.at(refined);
				this->cells.erase(refined);
			}

			// add children of refined cells into the grid
			std::vector<uint64_t> children = this->get_all_children(refined);
			BOOST_FOREACH(uint64_t child, children) {
				this->cell_process[child] = process_of_refined;

				if (this->comm.rank() == process_of_refined) {
					this->cells[child];
					this->neighbours[child];
					this->neighbours_to[child];
					new_cells.push_back(child);
				}
			}

			// children of refined cells inherit their pin request status
			if (this->pin_requests.count(refined) > 0) {
				BOOST_FOREACH(uint64_t child, children) {
					this->pin_requests[child] = this->pin_requests.at(refined);
				}
				this->pin_requests.erase(refined);
			}
			if (this->new_pin_requests.count(refined) > 0) {
				BOOST_FOREACH(uint64_t child, children) {
					this->new_pin_requests[child] = this->new_pin_requests.at(refined);
				}
				this->new_pin_requests.erase(refined);
			}

			// children of refined cells inherit their weight
			if (this->comm.rank() == process_of_refined
			&& this->cell_weights.count(refined) > 0) {
				BOOST_FOREACH(uint64_t child, children) {
					this->cell_weights[child] = this->cell_weights.at(refined);
				}
				this->cell_weights.erase(refined);
			}

			// use local neighbour lists to find cells whose neighbour lists have to updated
			if (this->comm.rank() == process_of_refined) {
				// update the neighbour lists of created local cells
				BOOST_FOREACH(uint64_t child, children) {
					update_neighbours.insert(child);
				}

				// update neighbour lists of all the parent's neighbours
				BOOST_FOREACH(uint64_t neighbour, this->neighbours.at(refined)) {
					if (neighbour == 0) {
						continue;
					}

					if (this->cell_process.at(neighbour) == this->comm.rank()) {
						update_neighbours.insert(neighbour);
					}
				}

				BOOST_FOREACH(uint64_t neighbour_to, this->neighbours_to.at(refined)) {
					if (this->cell_process.at(neighbour_to) == this->comm.rank()) {
						update_neighbours.insert(neighbour_to);
					}
				}
			}

			// without using local neighbour lists figure out rest of the neighbour lists that need updating
			BOOST_FOREACH(uint64_t with_remote_neighbour, this->cells_with_remote_neighbours) {
				if (this->is_neighbour(with_remote_neighbour, refined)
				|| this->is_neighbour(refined , with_remote_neighbour)) {
					update_neighbours.insert(with_remote_neighbour);
				}
			}
		}

		// needed for checking which neighbourhoods to update due to unrefining
		boost::unordered_set<uint64_t> parents_of_unrefined;

		// initially only one sibling is recorded per process when unrefining, insert the rest of them now
		boost::unordered_set<uint64_t> all_to_unrefine;
		BOOST_FOREACH(uint64_t unrefined, this->cells_to_unrefine) {

			const uint64_t parent_of_unrefined = this->get_parent(unrefined);
			#ifdef DEBUG
			if (parent_of_unrefined == 0) {
				std::cerr << __FILE__ << ":" << __LINE__ << " Invalid parent cell" << std::endl;
				abort();
			}

			if (parent_of_unrefined == unrefined) {
				std::cerr << __FILE__ << ":" << __LINE__
					<< " Cell " << unrefined
					<< " has no parent"
					<< std::endl;
				abort();
			}
			#endif

			parents_of_unrefined.insert(parent_of_unrefined);

			const std::vector<uint64_t> siblings = this->get_all_children(parent_of_unrefined);
			all_to_unrefine.insert(siblings.begin(), siblings.end());
		}

		// unrefines
		BOOST_FOREACH(uint64_t unrefined, all_to_unrefine) {

			const uint64_t parent_of_unrefined = this->get_parent(unrefined);
			const int process_of_parent = this->cell_process.at(parent_of_unrefined);
			const int process_of_unrefined = this->cell_process.at(unrefined);

			// remove unrefined cells and their siblings from the grid, but don't remove user data yet
			this->cell_process.erase(unrefined);
			update_neighbours.erase(unrefined);
			this->pin_requests.erase(unrefined);
			this->new_pin_requests.erase(unrefined);
			this->cell_weights.erase(unrefined);

			// don't send unrefined cells' user data to self
			if (this->comm.rank() == process_of_unrefined
			&& this->comm.rank() == process_of_parent) {
				// TODO move data instead of copying
				this->unrefined_cell_data[unrefined] = this->cells.at(unrefined);
				this->cells.erase(unrefined);

			// send user data of removed cell to the parent's process
			} else if (this->comm.rank() == process_of_unrefined) {
				this->cells_to_send[process_of_parent].push_back(unrefined);

			// receive user data of removed cell from its process
			} else if (this->comm.rank() == process_of_parent) {
				this->cells_to_receive[process_of_unrefined].push_back(unrefined);
			}
		}

		this->start_user_data_transfers(
		#ifdef DCCRG_SEND_SINGLE_CELLS
		this->unrefined_cell_data
		#else
		#ifdef DCCRG_CELL_DATA_SIZE_FROM_USER
		this->unrefined_cell_data
		#endif
		#endif
		);

		// update data for parents (and their neighborhood) of unrefined cells
		BOOST_FOREACH(uint64_t parent, parents_of_unrefined) {

			/* TODO: skip unrefined cells far enough away
			std::vector<uint64_t> children = this->get_all_children(*parent);
			*/

			const std::vector<uint64_t> new_neighbours_of = this->find_neighbours_of(parent);
			BOOST_FOREACH(uint64_t neighbour, new_neighbours_of) {

				if (neighbour == 0) {
					continue;
				}

				if (this->cell_process.at(neighbour) == this->comm.rank()) {
					update_neighbours.insert(neighbour);
				}
			}

			const std::vector<uint64_t> new_neighbours_to = this->find_neighbours_to(parent);
			BOOST_FOREACH(uint64_t neighbour, new_neighbours_to) {
				if (this->cell_process.at(neighbour) == this->comm.rank()) {
					update_neighbours.insert(neighbour);
				}
			}

			// add user data and neighbor lists of local parents of unrefined cells
			if (this->cell_process.at(parent) == this->comm.rank()) {
				this->cells[parent];
				this->neighbours[parent] = new_neighbours_of;
				this->neighbours_to[parent] = new_neighbours_to;
			}
		}

		// update neighbour lists of cells affected by refining / unrefining
		BOOST_FOREACH(uint64_t cell, update_neighbours) {
			this->update_neighbours(cell);
		}

		// remove neighbour lists of added cells' parents
		BOOST_FOREACH(uint64_t refined, this->cells_to_refine) {

			if (this->cell_process.at(refined) == this->comm.rank()) {

				#ifdef DEBUG
				if (this->neighbours.count(refined) == 0) {
					std::cerr << __FILE__ << ":" << __LINE__
						<< " Neighbour list for cell " << refined
						<< " doesn't exist"
						<< std::endl;
					abort();
				}

				if (this->neighbours_to.count(refined) == 0) {
					std::cerr << __FILE__ << ":" << __LINE__
						<< " Neighbour_to list for cell " << refined
						<< " doesn't exist"
						<< std::endl;
					abort();
				}
				#endif

				this->neighbours.erase(refined);
				this->neighbours_to.erase(refined);
			}
		}

		// remove neighbour lists of removed cells
		BOOST_FOREACH(uint64_t unrefined, all_to_unrefine) {
			this->neighbours.erase(unrefined);
			this->neighbours_to.erase(unrefined);
		}

		this->update_remote_neighbour_info();

		#ifdef DEBUG
		if (!this->verify_neighbours()) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Neighbour lists are inconsistent" << std::endl;
			exit(EXIT_FAILURE);
		}
		#endif

		this->wait_user_data_transfer_receives(
		#ifndef DCCRG_SEND_SINGLE_CELLS
		#ifndef DCCRG_CELL_DATA_SIZE_FROM_USER
		this->unrefined_cell_data
		#endif
		#endif
		);
		this->wait_user_data_transfer_sends();
		this->cells_to_send.clear();
		this->cells_to_receive.clear();

		#ifdef DEBUG
		if (!this->verify_user_data()) {
			std::cerr << __FILE__ << ":" << __LINE__ << " virhe" << std::endl;
			exit(EXIT_FAILURE);
		}
		#endif

		// remove user data of unrefined cells from this->cells
		for (boost::unordered_set<uint64_t>::const_iterator unrefined = all_to_unrefine.begin(); unrefined != all_to_unrefine.end(); unrefined++) {
			this->cells.erase(*unrefined);
		}

		this->cells_to_refine.clear();
		this->cells_to_unrefine.clear();

		this->recalculate_neighbour_update_send_receive_lists();

		return new_cells;
	}


	/*!
	Starts user data transfers between processes based on cells_to_send and cells_to_receive.

	User data arriving to this process is saved in given destination.
	*/
	void start_user_data_transfers(
	#ifdef DCCRG_SEND_SINGLE_CELLS
	boost::unordered_map<uint64_t, UserData>& destination
	#else
	#ifdef DCCRG_CELL_DATA_SIZE_FROM_USER
	boost::unordered_map<uint64_t, UserData>& destination
	#endif
	#endif
	)
	{
		#ifdef DCCRG_SEND_SINGLE_CELLS

		// post all receives, messages are unique between different senders so just iterate over processes in random order
		for (boost::unordered_map<int, std::vector<std::pair<uint64_t, int> > >::const_iterator
			sender = this->cells_to_receive.begin();
			sender != this->cells_to_receive.end();
			sender++
		) {
			#ifdef DEBUG
			if (sender->first == this->comm.rank()
			&& sender->second.size() > 0) {
				std::cerr << __FILE__ << ":" << __LINE__
					<< " Process " << this->comm.rank()
					<< " trying to transfer to self"
					<< std::endl;
				abort();
			}
			#endif

			for (std::vector<std::pair<uint64_t, int> >::const_iterator
				item = sender->second.begin();
				item != sender->second.end();
				item++
			) {
				#ifdef DCCRG_CELL_DATA_SIZE_FROM_USER
				this->receive_requests[sender->first].push_back(MPI_Request());

				#ifdef DCCRG_USER_MPI_DATA_TYPE
				MPI_Datatype user_datatype = destination[item->first].mpi_datatype();
				MPI_Type_commit(&user_datatype);
				#endif

				MPI_Irecv(
					destination[item->first].at(),
					#ifdef DCCRG_USER_MPI_DATA_TYPE
					1,
					user_datatype,
					#else
					UserData::size(),
					MPI_BYTE,
					#endif
					sender->first,
					item->second,
					this->comm,
					&(this->receive_requests[sender->first].back())
				);

				#ifdef DCCRG_USER_MPI_DATA_TYPE
				MPI_Type_free(&user_datatype);
				#endif

				#else // ifdef DCCRG_CELL_DATA_SIZE_FROM_USER

				this->receive_requests[sender->first].push_back(
					this->comm.irecv(
						sender->first,
						item->second,
						destination[item->first]
					)
				);
				#endif
			}
		}

		// post all sends
		for (boost::unordered_map<int, std::vector<std::pair<uint64_t, int> > >::const_iterator
			receiver = this->cells_to_send.begin();
			receiver != this->cells_to_send.end();
			receiver++
		) {
			#ifdef DEBUG
			if (receiver->first == this->comm.rank()
			&& receiver->second.size() > 0) {
				std::cerr << __FILE__ << ":" << __LINE__ << " Trying to transfer to self" << std::endl;
				abort();
			}
			#endif

			for (std::vector<std::pair<uint64_t, int> >::const_iterator
				item = receiver->second.begin();
				item != receiver->second.end();
				item++
			) {
				#ifdef DCCRG_CELL_DATA_SIZE_FROM_USER
				this->send_requests[receiver->first].push_back(MPI_Request());

				#ifdef DCCRG_USER_MPI_DATA_TYPE
				MPI_Datatype user_datatype = this->cells.at(item->first).mpi_datatype();
				MPI_Type_commit(&user_datatype);
				#endif

				MPI_Isend(
					this->cells.at(item->first).at(),
					#ifdef DCCRG_USER_MPI_DATA_TYPE
					1,
					user_datatype,
					#else
					UserData::size(),
					MPI_BYTE,
					#endif
					receiver->first,
					item->second,
					this->comm,
					&(this->send_requests[receiver->first].back())
				);

				#ifdef DCCRG_USER_MPI_DATA_TYPE
				MPI_Type_free(&user_datatype);
				#endif

				#else

				this->send_requests[receiver->first].push_back(
					this->comm.isend(
						receiver->first,
						item->second,
						this->cells.at(item->first)
					)
				);
				#endif
			}
		}

		// all user data is sent using one MPI message / process
		#else	// ifdef DCCRG_SEND_SINGLE_CELLS

		#ifdef DCCRG_CELL_DATA_SIZE_FROM_USER
		// receive one MPI datatype per process
		for (boost::unordered_map<int, std::vector<uint64_t> >::iterator
			sender = this->cells_to_receive.begin();
			sender != this->cells_to_receive.end();
			sender++
		) {
			std::sort(sender->second.begin(), sender->second.end());

			// reserve space for incoming user data at our end
			for (uint64_t i = 0; i < sender->second.size(); i++) {
				destination[sender->second[i]];
			}

			// get displacements in bytes for incoming user data
			std::vector<MPI_Aint> displacements(sender->second.size(), 0);
			for (uint64_t i = 0; i < sender->second.size(); i++) {
				displacements[i] = (uint8_t*) destination.at(sender->second[i]).at() - (uint8_t*) destination.at(sender->second[0]).at();
			}

			MPI_Datatype receive_datatype;

			#ifdef DCCRG_USER_MPI_DATA_TYPE
			std::vector<int> block_lengths(displacements.size(), 1);
			std::vector<MPI_Datatype> datatypes(displacements.size());
			for (uint64_t i = 0; i < sender->second.size(); i++) {
				datatypes[i] = destination.at(sender->second[i]).mpi_datatype();
			}
			#else
			std::vector<int> block_lengths(displacements.size(), UserData::size());
			#endif

			#ifdef DCCRG_USER_MPI_DATA_TYPE
			MPI_Type_create_struct(
				displacements.size(),
				&block_lengths[0],
				&displacements[0],
				&datatypes[0],
				&receive_datatype
			);
			#else
			MPI_Type_create_hindexed(
				displacements.size(),
				&block_lengths[0],
				&displacements[0],
				MPI_BYTE,
				&receive_datatype
			);
			#endif

			MPI_Type_commit(&receive_datatype);

			int receive_tag = sender->first * this->comm.size() + this->comm.rank();

			this->receive_requests[sender->first].push_back(MPI_Request());

			MPI_Irecv(
				destination.at(sender->second[0]).at(),
				1,
				receive_datatype,
				sender->first,
				receive_tag,
				this->comm,
				&(this->receive_requests[sender->first].back())
			);

			MPI_Type_free(&receive_datatype);
		}

		// send one MPI datatype per process
		for (boost::unordered_map<int, std::vector<uint64_t> >::iterator
			receiver = this->cells_to_send.begin();
			receiver != this->cells_to_send.end();
			receiver++
		) {
			std::sort(receiver->second.begin(), receiver->second.end());

			// get displacements in bytes for outgoing user data
			std::vector<MPI_Aint> displacements(receiver->second.size(), 0);
			for (uint64_t i = 0; i < receiver->second.size(); i++) {
				displacements[i] = (uint8_t*) this->cells.at(receiver->second[i]).at() - (uint8_t*) this->cells.at(receiver->second[0]).at();
			}

			MPI_Datatype send_datatype;

			#ifdef DCCRG_USER_MPI_DATA_TYPE
			std::vector<int> block_lengths(displacements.size(), 1);
			std::vector<MPI_Datatype> datatypes(displacements.size());
			for (uint64_t i = 0; i < receiver->second.size(); i++) {
				datatypes[i] = this->cells.at(receiver->second[i]).mpi_datatype();
			}
			#else
			std::vector<int> block_lengths(displacements.size(), UserData::size());
			#endif

			#ifdef DCCRG_USER_MPI_DATA_TYPE
			MPI_Type_create_struct(
				displacements.size(),
				&block_lengths[0],
				&displacements[0],
				&datatypes[0],
				&send_datatype
			);
			#else
			MPI_Type_create_hindexed(
				displacements.size(),
				&block_lengths[0],
				&displacements[0],
				MPI_BYTE,
				&send_datatype
			);
			#endif

			MPI_Type_commit(&send_datatype);

			int send_tag = this->comm.rank() * this->comm.size() + receiver->first;

			this->send_requests[receiver->first].push_back(MPI_Request());

			MPI_Isend(
				this->cells.at(receiver->second[0]).at(),
				1,
				send_datatype,
				receiver->first,
				send_tag,
				this->comm,
				&(this->send_requests[receiver->first].back())
			);

			MPI_Type_free(&send_datatype);
		}

		#else	// ifdef DCCRG_CELL_DATA_SIZE_FROM_USER

		// post all receives
		for (int sender = 0; sender < this->comm.size(); sender++) {

			if (sender == this->comm.rank()) {
				continue;
			}

			if (this->cells_to_receive.count(sender) == 0) {
				// no data to send / receive
				continue;
			}

			int receive_tag = sender * this->comm.size() + this->comm.rank();

			this->receive_requests[sender].push_back(
				this->comm.irecv(
					sender,
					receive_tag,
					this->incoming_data[sender]
				)
			);
		}

		// gather all data to send
		for (int receiver = 0; receiver < this->comm.size(); receiver++) {

			if (receiver == this->comm.rank()) {
				// don't send to self
				continue;
			}

			if (this->cells_to_send.count(receiver) == 0) {
				// no data to send / receive
				continue;
			}

			std::sort(this->cells_to_send.at(receiver).begin(), this->cells_to_send.at(receiver).end());
			// construct the outgoing data vector
			BOOST_FOREACH(uint64_t cell, this->cells_to_send.at(receiver)) {
				UserData* user_data = (*this)[cell];
				assert(user_data != NULL);
				this->outgoing_data[receiver].push_back(*user_data);
			}
		}

		// post all sends
		for (int receiver = 0; receiver < this->comm.size(); receiver++) {

			if (receiver == this->comm.rank()) {
				continue;
			}

			if (this->cells_to_send.count(receiver) == 0) {
				// no data to send / receive
				continue;
			}

			int send_tag = this->comm.rank() * this->comm.size() + receiver;

			this->send_requests[receiver].push_back(
				this->comm.isend(
					receiver,
					send_tag,
					this->outgoing_data[receiver]
				)
			);
		}
		#endif	// ifdef DCCRG_CELL_DATA_SIZE_FROM_USER
		#endif	// ifdef DCCRG_SEND_SINGLE_CELLS
	}


	/*!
	Waits for the receives of user data transfers between processes to complete.

	User data arriving to this process is saved in given destination.
	*/
	void wait_user_data_transfer_receives(
	#ifndef DCCRG_SEND_SINGLE_CELLS
	#ifndef DCCRG_CELL_DATA_SIZE_FROM_USER
	boost::unordered_map<uint64_t, UserData>& destination
	#endif
	#endif
	)
	{
		#ifdef DCCRG_CELL_DATA_SIZE_FROM_USER

		for (boost::unordered_map<int, std::vector<MPI_Request> >::iterator
			process = this->receive_requests.begin();
			process != this->receive_requests.end();
			process++
		) {
			std::vector<MPI_Status> statuses;
			statuses.resize(process->second.size());

			if (MPI_Waitall(process->second.size(), &(process->second[0]), &(statuses[0])) != MPI_SUCCESS) {
				BOOST_FOREACH(MPI_Status status, statuses) {
					if (status.MPI_ERROR != MPI_SUCCESS) {
						std::cerr << "MPI receive failed from process " << status.MPI_SOURCE
							<< " with tag " << status.MPI_TAG
							<< std::endl;
					}
				}
			}
		}

		#else	// ifdef DCCRG_CELL_DATA_SIZE_FROM_USER

		for (boost::unordered_map<int, std::vector<boost::mpi::request> >::iterator
			process = this->receive_requests.begin();
			process != this->receive_requests.end();
			process++
		) {
			boost::mpi::wait_all(process->second.begin(), process->second.end());
		}

		#ifndef DCCRG_SEND_SINGLE_CELLS

		// incorporate received data
		for (typename boost::unordered_map<int, std::vector<UserData> >::const_iterator
			sender = this->incoming_data.begin();
			sender != this->incoming_data.end();
			sender++
		) {
			std::sort(this->cells_to_receive.at(sender->first).begin(), this->cells_to_receive.at(sender->first).end());

			int i = 0;
			BOOST_FOREACH(uint64_t cell, this->cells_to_receive.at(sender->first)) {
				// TODO move data instead of copying
				destination[cell] = this->incoming_data.at(sender->first)[i];
				i++;
			}
		}
		this->incoming_data.clear();

		#endif	// ifndef DCCRG_SEND_SINGLE_CELLS
		#endif	// ifdef DCCRG_CELL_DATA_SIZE_FROM_USER

		this->receive_requests.clear();
	}


	/*!
	Waits for the sends of user data transfers between processes to complete.
	*/
	void wait_user_data_transfer_sends(void)
	{
		#ifdef DCCRG_CELL_DATA_SIZE_FROM_USER

		for (boost::unordered_map<int, std::vector<MPI_Request> >::iterator
			process = this->send_requests.begin();
			process != this->send_requests.end();
			process++
		) {
			std::vector<MPI_Status> statuses;
			statuses.resize(process->second.size());

			if (MPI_Waitall(process->second.size(), &(process->second[0]), &(statuses[0])) != MPI_SUCCESS) {
				BOOST_FOREACH(MPI_Status status, statuses) {
					if (status.MPI_ERROR != MPI_SUCCESS) {
						std::cerr << "MPI receive failed from process " << status.MPI_SOURCE
							<< " with tag " << status.MPI_TAG
							<< std::endl;
					}
				}
			}
		}

		#else	// ifdef DCCRG_CELL_DATA_SIZE_FROM_USER

		for (boost::unordered_map<int, std::vector<boost::mpi::request> >::iterator
			process = this->send_requests.begin();
			process != this->send_requests.end();
			process++
		) {
			boost::mpi::wait_all(process->second.begin(), process->second.end());
		}

		#ifndef DCCRG_SEND_SINGLE_CELLS

		this->outgoing_data.clear();

		#endif	// ifndef DCCRG_SEND_SINGLE_CELLS
		#endif	// ifdef DCCRG_CELL_DATA_SIZE_FROM_USER

		this->send_requests.clear();
	}


	/*!
	Returns true if cells with given index properties overlap.

	Sizes are also given in indices.
	*/
	bool indices_overlap(const uint64_t index1, const uint64_t size1, const uint64_t index2, const uint64_t size2) const
	{
		#ifdef DEBUG
		if (index1 >= this->get_x_length() * (uint64_t(1) << this->max_refinement_level)
		&& index1 >= this->get_y_length() * (uint64_t(1) << this->max_refinement_level)
		&& index1 >= this->get_z_length() * (uint64_t(1) << this->max_refinement_level)) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Invalid index given" << std::endl;
			exit(EXIT_FAILURE);
		}

		if (index2 >= this->get_x_length() * (uint64_t(1) << this->max_refinement_level)
		&& index2 >= this->get_y_length() * (uint64_t(1) << this->max_refinement_level)
		&& index2 >= this->get_z_length() * (uint64_t(1) << this->max_refinement_level)) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Invalid index given" << std::endl;
			exit(EXIT_FAILURE);
		}

		if (size1 > (uint64_t(1) << this->max_refinement_level)) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Invalid size given" << std::endl;
			exit(EXIT_FAILURE);
		}

		if (size2 > (uint64_t(1) << this->max_refinement_level)) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Invalid size given" << std::endl;
			exit(EXIT_FAILURE);
		}
		#endif

		if (index1 + size1 > index2 && index1 < index2 + size2) {
			return true;
		} else {
			return false;
		}
	}

	/*!
	Same as the uint64_t version but in 3d, returns true only if all indices overlap.
	*/
	bool indices_overlap(const Types<3>::indices_t indices1, const uint64_t size1, const Types<3>::indices_t indices2, const uint64_t size2) const
	{
		for (int i = 0; i < 3; i++) {
			if (indices1[i] + size1 <= indices2[i] || indices1[i] >= indices2[i] + size2) {
				return false;
			}
		}
		return true;
	}

	/*!
	Returns true if x indices of given cells overlap, even if they don't exist
	*/
	bool x_indices_overlap(const uint64_t cell1, const uint64_t cell2) const
	{
		assert(cell1 > 0);
		assert(cell1 <= this->last_cell);
		assert(cell2 > 0);
		assert(cell2 <= this->last_cell);

		const uint64_t index1 = this->get_x_index(cell1);
		const uint64_t index2 = this->get_x_index(cell2);
		const uint64_t size1 = this->get_cell_size_in_indices(cell1);
		const uint64_t size2 = this->get_cell_size_in_indices(cell2);

		return this->indices_overlap(index1, size1, index2, size2);
	}

	/*!
	Returns true if y indices of given cells overlap, even if they don't exist
	*/
	bool y_indices_overlap(const uint64_t cell1, const uint64_t cell2) const
	{
		assert(cell1 > 0);
		assert(cell1 <= this->last_cell);
		assert(cell2 > 0);
		assert(cell2 <= this->last_cell);

		const uint64_t index1 = this->get_y_index(cell1);
		const uint64_t index2 = this->get_y_index(cell2);
		const uint64_t size1 = this->get_cell_size_in_indices(cell1);
		const uint64_t size2 = this->get_cell_size_in_indices(cell2);

		return this->indices_overlap(index1, size1, index2, size2);
	}

	/*!
	Returns true if z indices of given cells overlap, even if they don't exist
	*/
	bool z_indices_overlap(const uint64_t cell1, const uint64_t cell2) const
	{
		assert(cell1 > 0);
		assert(cell1 <= this->last_cell);
		assert(cell2 > 0);
		assert(cell2 <= this->last_cell);

		const uint64_t index1 = this->get_z_index(cell1);
		const uint64_t index2 = this->get_z_index(cell2);
		const uint64_t size1 = this->get_cell_size_in_indices(cell1);
		const uint64_t size2 = this->get_cell_size_in_indices(cell2);

		return this->indices_overlap(index1, size1, index2, size2);
	}


	/*!
	Returns the number of directions in which given cells' indices overlap
	Returns 0 if even one of given cells doesn't exist
	*/
	int overlapping_indices(const uint64_t cell1, const uint64_t cell2) const
	{
		#ifdef DEBUG
		if (cell1 == 0) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Invalid cell given" << std::endl;
			abort();
		}
		if (cell2 == 0) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Invalid cell given" << std::endl;
			abort();
		}

		if (cell1 > this->last_cell) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Invalid cell given" << std::endl;
			abort();
		}
		if (cell2 > this->last_cell) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Invalid cell given" << std::endl;
			abort();
		}
		#endif

		if (this->cell_process.count(cell1) == 0 || this->cell_process.count(cell2) == 0) {
			return 0;
		}

		const Types<3>::indices_t indices1 = this->get_indices(cell1);
		const Types<3>::indices_t indices2 = this->get_indices(cell2);

		const uint64_t size1 = this->get_cell_size_in_indices(cell1);
		const uint64_t size2 = this->get_cell_size_in_indices(cell2);

		int ret = 0;
		if (this->indices_overlap(indices1[0], size1, indices2[0], size2)) {
			ret++;
		}
		if (this->indices_overlap(indices1[1], size1, indices2[1], size2)) {
			ret++;
		}
		if (this->indices_overlap(indices1[2], size1, indices2[2], size2)) {
			ret++;
		}

		return ret;
	}


	/*!
	Returns the smallest existing cell at given indices between given refinement levels inclusive.

	Returns error_cell if no cell between given refinement ranges exists or an index is outside of
	the grid or minimum_refinement_level > maximum_refinement_level.
	*/
	uint64_t get_existing_cell(
		const Types<3>::indices_t& indices,
		const int minimum_refinement_level,
		const int maximum_refinement_level
	) const
	{
		if (indices[0] >= this->x_length * (uint64_t(1) << this->max_refinement_level)) {
			return error_cell;
		}

		if (indices[1] >= this->y_length * (uint64_t(1) << this->max_refinement_level)) {
			return error_cell;
		}

		if (indices[2] >= this->z_length * (uint64_t(1) << this->max_refinement_level)) {
			return error_cell;
		}

		if (minimum_refinement_level > maximum_refinement_level) {
			return error_cell;
		}

		int average_refinement_level = (maximum_refinement_level + minimum_refinement_level) / 2;
		const uint64_t average_cell = this->get_cell_from_indices(indices, average_refinement_level);

		// use binary search recursively (assumes that all cells refine to 8 children)
		if (this->cell_process.count(average_cell) == 0) {

			// search for larger cell
			if (average_refinement_level > minimum_refinement_level) {

				uint64_t larger_cell = this->get_existing_cell(indices, minimum_refinement_level, average_refinement_level - 1);

				if (this->cell_process.count(larger_cell) == 0) {
					return 0;
				} else {
					return larger_cell;
				}
			} else {
				return 0;
			}
		} else {
			// search for smaller cell
			if (average_refinement_level < maximum_refinement_level) {
				uint64_t smaller_cell = this->get_existing_cell(indices, average_refinement_level + 1, maximum_refinement_level);

				if (this->cell_process.count(smaller_cell) == 0) {
					return average_cell;
				} else {
					return smaller_cell;
				}
			} else {
				return average_cell;
			}
		}
	}


	/*!
	Returns the number of values needed to represent the coordinate of a cell
	*/
	static int get_grid_dimensionality(void* /*data*/, int* error)
	{
		*error = ZOLTAN_OK;
		return 3;
	}


	// TODO: Zoltan assumes global ids are integers, which works as long as there are less than 2^32 cells in the grid

	/*!
	Fills geom_vec with the coordinates of cells given in global_id
	*/
	static void fill_with_cell_coordinates(void *data, int /*global_id_size*/, int /*local_id_size*/, int number_of_cells, ZOLTAN_ID_PTR global_ids, ZOLTAN_ID_PTR /*local_ids*/, int /*number_of_dimensions*/, double *geom_vec, int *error)
	{
		Dccrg<UserData, UserGeometry>* dccrg_instance = reinterpret_cast<Dccrg<UserData, UserGeometry> *>(data);
		*error = ZOLTAN_OK;

		for (int i = 0; i < number_of_cells; i++) {
			uint64_t cell = uint64_t(global_ids[i]);
			if (dccrg_instance->cells.count(cell) == 0) {
				*error = ZOLTAN_FATAL;
				std::cerr << "Process " << dccrg_instance->comm.rank() << ": Zoltan wanted the coordinates of a non-existing cell " << cell << std::endl;
				return;
			}

			geom_vec[3 * i + 0] = dccrg_instance->get_cell_x(cell);
			geom_vec[3 * i + 1] = dccrg_instance->get_cell_y(cell);
			geom_vec[3 * i + 2] = dccrg_instance->get_cell_z(cell);
		}
	}


	/*!
	Returns the number of cells on this process
	*/
	static int get_number_of_cells(void* data, int* error)
	{
		Dccrg<UserData, UserGeometry>* dccrg_instance = reinterpret_cast<Dccrg<UserData, UserGeometry> *>(data);
		*error = ZOLTAN_OK;
		return dccrg_instance->cells.size();
	}


	/*!
	Writes all cell ids on this process to the global_ids array
	*/
	static void fill_cell_list(void* data, int /*global_id_size*/, int /*local_id_size*/, ZOLTAN_ID_PTR global_ids, ZOLTAN_ID_PTR /*local_ids*/, int number_of_weights_per_object, float* object_weights, int* error)
	{
		Dccrg<UserData, UserGeometry>* dccrg_instance = reinterpret_cast<Dccrg<UserData, UserGeometry> *>(data);
		*error = ZOLTAN_OK;

		int i = 0;
		BOOST_FOREACH(cell_and_data_pair_t item, dccrg_instance->cells) {

			#ifdef DEBUG
			if (item.first == 0) {
				std::cerr << "User data exist for an illegal cell" << std::endl;
				abort();
			}
			#endif

			global_ids[i] = item.first;

			if (number_of_weights_per_object > 0) {
				if (dccrg_instance->cell_weights.count(item.first) > 0) {
					object_weights[i] = dccrg_instance->cell_weights.at(item.first);
				} else {
					object_weights[i] = 1;
				}
			}

			i++;
		}
	}


	/*!
	Writes the number of neighbours into number_of_neighbours for all cells given in global_ids with length number_of_cells
	*/
	static void fill_number_of_neighbours_for_cells(void* data, int /*global_id_size*/, int /*local_id_size*/, int number_of_cells, ZOLTAN_ID_PTR global_ids, ZOLTAN_ID_PTR /*local_ids*/, int* number_of_neighbours, int* error)
	{
		Dccrg<UserData, UserGeometry>* dccrg_instance = reinterpret_cast<Dccrg<UserData, UserGeometry> *>(data);
		*error = ZOLTAN_OK;

		for (int i = 0; i < number_of_cells; i++) {
			uint64_t cell = uint64_t(global_ids[i]);
			if (dccrg_instance->cells.count(cell) == 0) {
				*error = ZOLTAN_FATAL;
				std::cerr << "Process " << dccrg_instance->comm.rank()
					<< ": Zoltan wanted the number of neighbours of a non-existing cell " << cell
					<< std::endl;
				return;
			}

			number_of_neighbours[i] = 0;
			BOOST_FOREACH(uint64_t neighbour, dccrg_instance->neighbours.at(cell)) {
				if (neighbour != 0
				/* Zoltan 3.501 crashes in hierarchial
				if a cell is a neighbor to itself */
				&& neighbour != cell) {
					number_of_neighbours[i]++;
				}
			}
		}
	}


	/*!
	Writes neighbour lists of given cells into neighbours, etc.
	*/
	static void fill_neighbour_lists(void* data, int /*global_id_size*/, int /*local_id_size*/, int number_of_cells, ZOLTAN_ID_PTR global_ids, ZOLTAN_ID_PTR /*local_ids*/, int* number_of_neighbours, ZOLTAN_ID_PTR neighbours, int* processes_of_neighbours, int number_of_weights_per_edge, float* edge_weights, int* error)
	{
		Dccrg<UserData, UserGeometry>* dccrg_instance = reinterpret_cast<Dccrg<UserData, UserGeometry> *>(data);
		*error = ZOLTAN_OK;

		int current_neighbour_number = 0;
		for (int i = 0; i < number_of_cells; i++) {
			uint64_t cell = uint64_t(global_ids[i]);
			if (dccrg_instance->cells.count(cell) == 0) {
				*error = ZOLTAN_FATAL;
				std::cerr << "Process " << dccrg_instance->comm.rank() << ": Zoltan wanted neighbour list of a non-existing cell " << cell << std::endl;
				return;
			}

			number_of_neighbours[i] = 0;

			BOOST_FOREACH(uint64_t neighbour, dccrg_instance->neighbours.at(cell)) {

				if (neighbour == 0
				/* Zoltan 3.501 crashes in hierarchial
				if a cell is a neighbor to itself */
				|| neighbour == cell) {
					continue;
				}

				number_of_neighbours[i]++;

				neighbours[current_neighbour_number] = neighbour;
				processes_of_neighbours[current_neighbour_number] = dccrg_instance->cell_process.at(neighbour);

				// weight of edge from cell to *neighbour
				if (number_of_weights_per_edge > 0) {
					edge_weights[current_neighbour_number] = 1.0;
				}

				current_neighbour_number++;
			}
		}
	}


	/*!
	Writes the number of hyperedges (self + one per neighbour cell) in the grid for all cells on this process.
	*/
	static void fill_number_of_hyperedges(void* data, int* number_of_hyperedges, int* number_of_connections, int* format, int* error)
	{
		Dccrg<UserData, UserGeometry>* dccrg_instance = reinterpret_cast<Dccrg<UserData, UserGeometry> *>(data);
		*error = ZOLTAN_OK;

		*number_of_hyperedges = dccrg_instance->cells.size();
		*format = ZOLTAN_COMPRESSED_EDGE;

		*number_of_connections = 0;
		BOOST_FOREACH(cell_and_data_pair_t item, dccrg_instance->cells) {

			(*number_of_connections)++;

			BOOST_FOREACH(uint64_t neighbour, dccrg_instance->neighbours.at(item.first)) {
				if (neighbour != 0
				/* Zoltan 3.501 crashes in hierarchial
				if a cell is a neighbor to itself */
				&& neighbour != item.first) {
					(*number_of_connections)++;
				}
			}
		}
	}


	/*!
	Writes the hypergraph in compressed edge format
	*/
	static void fill_hyperedge_lists(void* data, int /*global_id_size*/, int number_of_hyperedges, int number_of_connections, int format, ZOLTAN_ID_PTR hyperedges, int* hyperedge_connection_offsets, ZOLTAN_ID_PTR connections, int* error)
	{
		Dccrg<UserData, UserGeometry>* dccrg_instance = reinterpret_cast<Dccrg<UserData, UserGeometry> *>(data);
		*error = ZOLTAN_OK;

		if (format != ZOLTAN_COMPRESSED_EDGE) {
			std::cerr << "Only compressed edge format supported for hypergraph partitioning" << std::endl;
			*error = ZOLTAN_FATAL;
			return;
		}

		if ((unsigned int) number_of_hyperedges != dccrg_instance->cells.size()) {
			std::cerr << "Zoltan is expecting wrong number of hyperedges: " << number_of_hyperedges << " instead of " << dccrg_instance->cells.size() << std::endl;
			*error = ZOLTAN_FATAL;
			return;
		}

		int i = 0;
		int connection_number = 0;
		BOOST_FOREACH(cell_and_data_pair_t item, dccrg_instance->cells) {

			hyperedges[i] = item.first;
			hyperedge_connection_offsets[i] = connection_number;

			// add a connection to the cell itself from its hyperedge
			connections[connection_number++] = item.first;

			BOOST_FOREACH(uint64_t neighbour, dccrg_instance->neighbours.at(item.first)) {
				if (neighbour == 0
				/* Zoltan 3.501 crashes in hierarchial
				if a cell is a neighbor to itself */
				|| neighbour == item.first) {
					continue;
				}

				connections[connection_number++] = neighbour;
			}

			i++;
		}

		if (connection_number != number_of_connections) {
			std::cerr << "Zoltan is expecting wrong number of connections from hyperedges: " << number_of_connections
				<< " instead of " << connection_number
				<< std::endl;
			*error = ZOLTAN_FATAL;
			return;
		}
	}


	/*!
	Writes the number of hyperedge weights (one per hyperedge) on this process
	*/
	static void fill_number_of_edge_weights(void* data, int* number_of_edge_weights, int* error)
	{
		Dccrg<UserData, UserGeometry>* dccrg_instance = reinterpret_cast<Dccrg<UserData, UserGeometry> *>(data);
		*error = ZOLTAN_OK;

		*number_of_edge_weights = dccrg_instance->cells.size();
		return;
	}


	/*!
	Writes hyperedge weights (one per hyperedge) on this process
	*/
	static void fill_edge_weights(void* data, int /*global_id_size*/, int /*local_id_size*/, int number_of_hyperedges, int number_of_weights_per_hyperedge, ZOLTAN_ID_PTR hyperedges, ZOLTAN_ID_PTR /*hyperedges_local_ids*/, float* hyperedge_weights, int* error)
	{
		Dccrg<UserData, UserGeometry>* dccrg_instance = reinterpret_cast<Dccrg<UserData, UserGeometry> *>(data);
		*error = ZOLTAN_OK;

		if ((unsigned int) number_of_hyperedges != dccrg_instance->cells.size()) {
			std::cerr << "Zoltan is expecting wrong number of hyperedges: " << number_of_hyperedges << " instead of " << dccrg_instance->cells.size() << std::endl;
			*error = ZOLTAN_FATAL;
			return;
		}

		int i = 0;
		BOOST_FOREACH(cell_and_data_pair_t item, dccrg_instance->cells) {

			hyperedges[i] = item.first;

			if (number_of_weights_per_hyperedge > 0) {
				int number_of_hyperedges = 0;

				BOOST_FOREACH(uint64_t neighbour, dccrg_instance->neighbours.at(item.first)) {
					if (neighbour != 0
					/* Zoltan 3.501 crashes in hierarchial
					if a cell is a neighbor to itself */
					&& neighbour != item.first) {
						number_of_hyperedges++;
					}
				}

				hyperedge_weights[i] = 1.0 * number_of_hyperedges;
			}

			i++;
		}
	}


	/*!
	Returns the number of hierarchies to use for load balancing.
	*/
	static int get_number_of_load_balancing_hierarchies(void* data, int* error)
	{
		Dccrg<UserData, UserGeometry>* dccrg_instance = reinterpret_cast<Dccrg<UserData, UserGeometry> *>(data);
		*error = ZOLTAN_OK;
		return dccrg_instance->processes_per_part.size();
	}


	/*!
	Returns the part number of this process on given hierarchy level of load balancing.
	*/
	static int get_part_number(void* data, int level, int* error)
	{
		Dccrg<UserData, UserGeometry>* dccrg_instance = reinterpret_cast<Dccrg<UserData, UserGeometry> *>(data);

		if (level < 0 || level >= int(dccrg_instance->processes_per_part.size())) {
			std::cerr << "Zoltan wanted a part number for an invalid hierarchy level (should be [0, " << dccrg_instance->processes_per_part.size() - 1 << "]): " << level << std::endl;
			*error = ZOLTAN_FATAL;
			return -1;
		} else {
			*error = ZOLTAN_OK;
		}

		int process = dccrg_instance->comm.rank();
		int part;

		for (int i = 0; i <= level; i++) {
			part = process / dccrg_instance->processes_per_part[i];
			process %= dccrg_instance->processes_per_part[i];
		}

		return part;
	}


	/*!
	Sets the partitioning options of given zoltan instance for given level.
	*/
	static void set_partitioning_options(void* data, int level, struct Zoltan_Struct* zz, int* error)
	{
		if (zz == NULL) {
			std::cerr << "Zoltan gave a NULL pointer for zz" << std::endl;
			*error = ZOLTAN_FATAL;
			return;
		}

		Dccrg<UserData, UserGeometry>* dccrg_instance = reinterpret_cast<Dccrg<UserData, UserGeometry> *>(data);

		if (level < 0 || level >= int(dccrg_instance->processes_per_part.size())) {
			std::cerr << "Zoltan wanted partitioning options for an invalid hierarchy level (level should be between 0 and " << dccrg_instance->processes_per_part.size() - 1 << " inclusive): " << level << std::endl;
			*error = ZOLTAN_FATAL;
			return;
		} else {
			*error = ZOLTAN_OK;
		}

		for (boost::unordered_map<std::string, std::string>::const_iterator
			option = dccrg_instance->partitioning_options.at(level).begin();
			option != dccrg_instance->partitioning_options.at(level).end();
			option++
		) {
			Zoltan_Set_Param(zz, option->first.c_str(), option->second.c_str());
		}
	}



	#ifdef DEBUG
	/*!
	Returns false if the same cells don't exist on the same process for all processes.
	*/
	bool is_consistent(void)
	{
		// sort existing cells from this process
		std::vector<uint64_t> local_cells;
		local_cells.reserve(this->cell_process.size());
		for (typename boost::unordered_map<uint64_t, int>::const_iterator
			cell = this->cell_process.begin();
			cell != this->cell_process.end();
			cell++
		) {
			local_cells.push_back(cell->first);
		}
		sort(local_cells.begin(), local_cells.end());

		// processes of existing cells from this process
		std::vector<int> local_processes;
		local_processes.reserve(this->cell_process.size());
		for (std::vector<uint64_t>::const_iterator
			cell = local_cells.begin();
			cell != local_cells.end();
			cell++
		) {
			local_processes.push_back(this->cell_process[*cell]);
		}

		// compare the above between processes
		std::vector<std::vector<uint64_t> > all_cells;
		all_gather(this->comm, local_cells, all_cells);
		std::vector<std::vector<int> > all_processes;
		all_gather(this->comm, local_processes, all_processes);

		for (int process = 0; process < this->comm.size(); process++) {
			if (!std::equal(all_cells[process].begin(), all_cells[process].end(), all_cells[0].begin())) {
				std::cerr << __FILE__ << ":" << __LINE__
					<< " Grid has different cells between processes 0 and " << process
					<< std::endl;
				return false;
			}

			if (!std::equal(all_processes[process].begin(), all_processes[process].end(), all_processes[0].begin())) {
				std::cerr << __FILE__ << ":" << __LINE__
					<< " Grid's cells have different processes between processes 0 and " << process
					<< std::endl;
				return false;
			}
		}

		return true;
	}


	/*!
	Return false if neighbours lists of the given cell aren't consistent
	*/
	bool verify_neighbours(const uint64_t cell)
	{
		if (cell == 0) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Invalid cell given" << std::endl;
			return false;
		}

		if (cell > this->last_cell) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Cell " << cell << " shouldn't exist" << std::endl;
			return false;
		}

		if (this->cell_process.count(cell) == 0) {
			std::cerr << __FILE__ << ":" << __LINE__ << " Cell " << cell << " doesn't exist" << std::endl;
			return false;
		}

		if (cell == this->get_child(cell)) {

			if (this->neighbours.count(cell) == 0) {
				std::cerr << __FILE__ << ":" << __LINE__ << " No neighbour list for cell " << cell << std::endl;
				return false;
			}

			if (this->neighbours_to.count(cell) == 0) {
				std::cerr << __FILE__ << ":" << __LINE__ << " No neighbour_to list for cell " << cell << std::endl;
				return false;
			}

		} else {

			if (this->neighbours.count(cell) > 0) {
				std::cerr << __FILE__ << ":" << __LINE__ << " Neighbour list for cell " << cell << " shouldn't exist" << std::endl;
				return false;
			}

			if (this->neighbours_to.count(cell) > 0) {
				std::cerr << __FILE__ << ":" << __LINE__ << " Neighbour_to list for cell " << cell << " shouldn't exist" << std::endl;
				return false;
			}

		}

		if (cell != this->get_child(cell)) {
			return true;
		}

		// neighbours
		sort(this->neighbours.at(cell).begin(), this->neighbours.at(cell).end());
		std::vector<uint64_t> compare_neighbours = this->find_neighbours_of(cell);
		sort(compare_neighbours.begin(), compare_neighbours.end());

		if ((
			this->neighbours.at(cell).size() != compare_neighbours.size())
		|| (
			this->neighbours.at(cell).size() > 0
			&& compare_neighbours.size() > 0
			&& !std::equal(this->neighbours.at(cell).begin(), this->neighbours.at(cell).end(), compare_neighbours.begin()))
		) {
			std::cerr << "Process " << this->comm.rank()
				<< " neighbour counts for cell " << cell
				<< " (child of " << this->get_parent(cell)
				<< ") don't match "
				<< this->neighbours.at(cell).size() << ": ";

			BOOST_FOREACH(uint64_t c, this->neighbours.at(cell)) {
				std::cerr << c << " ";
			}
			std::cerr << ", should be (+ child of) " << compare_neighbours.size() << ": ";
			BOOST_FOREACH(uint64_t c, compare_neighbours) {
				std::cerr << c << "(" << this->get_parent(c) << ") ";
			}
			std::cerr << std::endl;
			return false;
		}

		// neighbours_to
		if (this->neighbours_to.at(cell).size() > 0) {
			sort(this->neighbours_to.at(cell).begin(), this->neighbours_to.at(cell).end());
		}
		std::vector<uint64_t> compare_neighbours_to = this->find_neighbours_to(cell);
		if (compare_neighbours_to.size() > 0) {
			sort(compare_neighbours_to.begin(), compare_neighbours_to.end());
		}

		if (!std::equal(this->neighbours_to.at(cell).begin(), this->neighbours_to.at(cell).end(), compare_neighbours_to.begin())) {
			std::cerr << "Process " << this->comm.rank()
				<< " neighbour_to counts for cell " << cell
				<< " (child of " << this->get_parent(cell)
				<< ") don't match: " << this->neighbours_to.at(cell).size()
				<< " (";

			BOOST_FOREACH(uint64_t c, this->neighbours_to.at(cell)) {
				std::cerr << c;
				if (c != this->get_child(c)) {
					std::cerr << " [has a child " << this->get_child(c) << "], ";
				} else {
					std::cerr << ", ";
				}
			}
			std::cerr << ") should be " << compare_neighbours_to.size() << " (";
			BOOST_FOREACH(uint64_t c, compare_neighbours_to) {
				std::cerr << c << ", ";
			}
			std::cerr << ")" << std::endl;
			return false;
		}

		return true;
	}


	/*!
	Returns false if neighbour lists on this process aren't consistent
	*/
	bool verify_neighbours(void)
	{
		for (typename boost::unordered_map<uint64_t, int>::const_iterator
			cell = this->cell_process.begin();
			cell != this->cell_process.end();
			cell++
		) {
			if (cell->second != this->comm.rank()) {
				continue;
			}

			if (!this->verify_neighbours(cell->first)) {
				return false;
			}
		}

		return true;
	}


	/*!
	Returns false if remote neighbour info for given cell is inconsistent.

	Remote neighbour info consists of cells_with_remote_neighbours and remote_cells_with_local_neighbours.
	*/
	bool verify_remote_neighbour_info(const uint64_t cell)
	{
		if (!this->verify_neighbours(cell)) {
			std::cerr << __FILE__ << ":" << __LINE__
				<< " Cell " << cell
				<< " has inconsistent neighbors"
				<< std::endl;
			return false;
		}

		if (cell != this->get_child(cell)) {
			std::cerr << __FILE__ << ":" << __LINE__
				<< " Cell " << cell << " has children"
				<< std::endl;
			return true;
		}

		std::vector<uint64_t> all_neighbours(this->neighbours.at(cell).begin(), this->neighbours.at(cell).end());
		all_neighbours.insert(all_neighbours.end(), this->neighbours_to.at(cell).begin(), this->neighbours_to.at(cell).end());

		BOOST_FOREACH(uint64_t neighbour, all_neighbours) {

			if (neighbour == 0) {
				continue;
			}

			if (this->cell_process.at(neighbour) != this->comm.rank()) {

				if (this->cells_with_remote_neighbours.count(cell) == 0) {
					std::cerr << __FILE__ << ":" << __LINE__
						<< " Local cell " << cell
						<< " should be in cells_with_remote_neighbours"
						<< std::endl;
					return false;
				}

				if (this->remote_cells_with_local_neighbours.count(neighbour) == 0) {
					std::cerr << __FILE__ << ":" << __LINE__
						<< " Remote cell " << neighbour
						<< " should be in remote_cells_with_local_neighbours"
						<< std::endl;
					return false;
				}
			}
		}

		return true;
	}


	/*!
	Returns false if remote neighbour info on this process is inconsistent.

	Remote neighbour info consists of cells_with_remote_neighbours and remote_cells_with_local_neighbours.
	*/
	bool verify_remote_neighbour_info(void)
	{
		for (boost::unordered_map<uint64_t, int>::const_iterator
			item = this->cell_process.begin();
			item != this->cell_process.end();
			item++
		) {

			if (item->first != this->get_child(item->first)) {
				continue;
			}

			// check whether this cell should be in remote_cells_with_local_neighbours
			if (item->second != this->comm.rank()) {

				bool should_be_in_remote_cells = false;

				BOOST_FOREACH(cell_and_data_pair_t cell, this->cells) {

					if (cell.first != this->get_child(cell.first)) {
						continue;
					}

					if (item->first == cell.first) {
						std::cerr << __FILE__ << ":" << __LINE__ << " Same cell." << std::endl;
						abort();
					}

					if (this->is_neighbour(item->first, cell.first)
					|| this->is_neighbour(cell.first, item->first)) {
						should_be_in_remote_cells = true;
					}
				}

				if (should_be_in_remote_cells) {

					if (this->remote_cells_with_local_neighbours.count(item->first) == 0) {
						std::cerr << __FILE__ << ":" << __LINE__
							<< " Remote cell " << item->first
							<< " should be in remote_cells_with_local_neighbours because:"
							<< std::endl;

						BOOST_FOREACH(cell_and_data_pair_t cell, this->cells) {
							if (item->first == cell.first) {
								std::cerr << __FILE__ << ":" << __LINE__ << " Same cell." << std::endl;
								abort();
							}

							if (this->is_neighbour(item->first, cell.first)
							|| this->is_neighbour(cell.first, item->first)) {
								std::cerr << "\tremote cell " << item->first
									<< " has a local neighbour " << cell.first
									<< std::endl;
							}
						}
						return false;
					}

				} else {

					if (this->remote_cells_with_local_neighbours.count(item->first) > 0) {
						std::cerr << __FILE__ << ":" << __LINE__
							<< " Remote cell " << item->first
							<< " should not be in remote_cells_with_local_neighbours"
							<< std::endl;
						return false;
					}
				}

			// check whether this cell should be in cells_with_remote_neighbour
			} else {

				bool no_remote_neighbour = true;

				// search in neighbours_of
				const std::vector<uint64_t> neighbours_of = this->find_neighbours_of(item->first);

				BOOST_FOREACH(uint64_t neighbour, neighbours_of) {

					if (neighbour == 0) {
						continue;
					}

					if (this->cell_process.at(neighbour) != this->comm.rank()) {
						no_remote_neighbour = false;
					}

					if (!this->is_neighbour(item->first, neighbour)) {
						std::cerr << __FILE__ << ":" << __LINE__
							<< " Cell " << neighbour
							<< " should be a neighbour of cell " << item->first
							<< std::endl;
						abort();
					}
				}

				// search in neighbours_to
				std::vector<uint64_t> neighbours_to = this->find_neighbours_to(item->first);
				BOOST_FOREACH(uint64_t neighbour, neighbours_to) {

					if (neighbour == 0) {
						continue;
					}

					if (this->cell_process.at(neighbour) != this->comm.rank()) {
						no_remote_neighbour = false;
					}

					if (!this->is_neighbour(neighbour, item->first)) {
						std::cerr << __FILE__ << ":" << __LINE__
							<< " Cell " << item->first
							<< " should be a neighbour of cell " << neighbour
							<< std::endl;
						exit(EXIT_FAILURE);
					}
				}

				if (no_remote_neighbour) {
					if (this->cells_with_remote_neighbours.count(item->first) > 0) {
						std::cerr << __FILE__ << ":" << __LINE__
							<< " Local cell " << item->first
							<< " should not be in cells_with_remote_neighbours"
							<< std::endl;
						return false;
					}
				} else {
					if (this->cells_with_remote_neighbours.count(item->first) == 0) {
						std::cerr << __FILE__ << ":" << __LINE__
							<< " Local cell " << item->first
							<< " should be in cells_with_remote_neighbours"
							<< std::endl;
						return false;
					}
				}
			}
		}

		return true;
	}


	/*!
	Returns true if user data exists for local cells.
	*/
	bool verify_user_data(void)
	{
		for (boost::unordered_map<uint64_t, int>::const_iterator
			item = this->cell_process.begin();
			item != this->cell_process.end();
			item++
		) {
			if (item->second == this->comm.rank()
			&& item->first == this->get_child(item->first)
			&& this->cells.count(item->first) == 0) {
				std::cerr << __FILE__ << ":" << __LINE__
					<< " User data for local cell " << item->first
					<< " should exist"
					<< std::endl;
				return false;
			}
			if (item->second != this->comm.rank()
			&& this->cells.count(item->first) > 0) {
				std::cerr << __FILE__ << ":" << __LINE__
					<< " User data for local cell " << item->first
					<< " shouldn't exist"
					<< std::endl;
				return false;
			}
		}

		return true;
	}


	/*!
	Returns true if all cells are where pin reqests should have placed them.
	*/
	bool pin_requests_succeeded(void)
	{
		for (boost::unordered_map<uint64_t, int>::const_iterator
			pin_request = this->pin_requests.begin();
			pin_request != this->pin_requests.end();
			pin_request++
		) {
			if (this->cell_process.at(pin_request->first) != pin_request->second) {
				std::cerr << __FILE__ << ":" << __LINE__
					<< " Cell " << pin_request->first
					<< " not at requested process " << pin_request->second
					<< " but at " << this->cell_process.at(pin_request->first)
					<< std::endl;
				return false;
			}
		}

		return true;
	}
	#endif

};

}	// namespace

#endif

